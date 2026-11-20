/* SPDX-License-Identifier: GPL-3.0-or-later
 * wsprdec.c - WSPR-2 decoder, freestanding C (no libc/libm/malloc/FFTW).
 *
 * Ported from WSJT-X wsprd (wsprd.c / wsprd_utils.c / fano.c / metric_tables.c,
 * Copyright 2001-2018 Joe Taylor K1JT & Steven Franke K9AN, GPL-3.0).  The
 * candidate search, sync_and_demodulate, noncoherent_sequence_detection,
 * subtract_signal2, the soft Fano decode with WSJT-X metric tables and the
 * message unpacking are faithful re-implementations of that code.  Replaced:
 * the giant readwavfile() FFT down-sampler -> a polyphase FIR down-converter
 * (mix at 1500 Hz, resample 8000->375), and FFTW -> an in-tree radix-2 512-pt
 * FFT; all memory comes from a caller buffer.  Default decode path only
 * (Fano, 3 passes, subtraction, jitter, block sizes); no Jelinek/OSD.
 *
 * Reduced-memory build (fits the IC-705 on-chip region, <=240 KiB): the 375 sps
 * I/Q is stored s16 (decode is scale-invariant; measured identical to float at
 * every SNR), the full spectrogram is NOT kept (find_candidates forms only
 * psavg; the coarse search recomputes a 17-bin band per candidate), and the
 * subtraction streams a 2*nfilt ci/cq ring.  All large per-call temporaries
 * live in the work arena (W->scr), keeping the task stack well under 12 KiB.
 * Work-arena peak ~210 KiB (see wspr_work_peak()); plus a small static context.
 */
#include "wsprdec.h"
#ifdef WSPR_DEBUG
#include <stdio.h>
#include <stdlib.h>
#define DBG(...) fprintf(stderr,__VA_ARGS__)
#else
#define DBG(...) do{}while(0)
#endif

/* re-encoder (from wsprenc.c) used for symbol subtraction */
extern int wspr_pack_message(const char *msg, uint8_t tones[162]);
int wspr_pack_for_subtract(const char *clp, uint8_t *chan);

/* ===================== freestanding helpers ============================ */
__attribute__((weak, optimize("no-tree-loop-distribute-patterns")))
void *memset(void *d, int c, __SIZE_TYPE__ n){uint8_t*p=d;while(n--)*p++=(uint8_t)c;return d;}
__attribute__((weak, optimize("no-tree-loop-distribute-patterns")))
void *memcpy(void *d, const void *s, __SIZE_TYPE__ n){uint8_t*p=d;const uint8_t*q=s;while(n--)*p++=*q++;return d;}

static float w_sqrtf(float x){ return x>0.0f?__builtin_sqrtf(x):0.0f; }
static float w_logf(float x){
    union{float f;uint32_t u;}v; int e; float m,s,s2;
    if(!(x>0.0f))return -1e30f;
    v.f=x; e=(int)((v.u>>23)&255u)-127; v.u=(v.u&0x7fffffu)|0x3f800000u; m=v.f;
    if(m>1.41421356f){m*=0.5f;e++;}
    s=(m-1.0f)/(m+1.0f); s2=s*s;
    return 2.0f*s*(1.0f+s2*(0.33333333f+s2*(0.2f+s2*(0.14285714f+s2*0.11111111f))))+(float)e*0.69314718f;
}
static float w_log10f(float x){ return 0.43429448f*w_logf(x); }
/* sin/cos (double arg, range reduced) */
static void w_sincos(double x, float *s, float *c){
    const double pio2=1.57079632679489661923;
    double q=x*(1.0/pio2),r,r2,sn,cs; int32_t n=(int32_t)(q>=0?q+0.5:q-0.5);
    r=x-(double)n*pio2; r2=r*r;
    sn=r*(1-r2/6*(1-r2/20*(1-r2/42*(1-r2/72*(1-r2/110*(1-r2/156))))));
    cs=1-r2/2*(1-r2/12*(1-r2/30*(1-r2/56*(1-r2/90*(1-r2/132)))));
    switch(n&3){
    case 0:*s=(float)sn;*c=(float)cs;break;
    case 1:*s=(float)cs;*c=(float)-sn;break;
    case 2:*s=(float)-sn;*c=(float)-cs;break;
    default:*s=(float)-cs;*c=(float)sn;break; }
}
static float w_sinf(double x){float s,c;w_sincos(x,&s,&c);return s;}
static float w_cosf(double x){float s,c;w_sincos(x,&s,&c);return c;}
static int32_t w_iround(float x){return (int32_t)(x>=0.0f?x+0.5f:x-0.5f);}
static uint32_t w_parity(uint32_t v){v^=v>>16;v^=v>>8;v^=v>>4;v^=v>>2;v^=v>>1;return v&1u;}
static int w_strlen(const char*s){int n=0;while(s[n])n++;return n;}
static void w_strcpy(char*d,const char*s){while((*d++=*s++));}
static int w_isalpha(char c){return (c>='A'&&c<='Z')||(c>='a'&&c<='z');}
static int w_isdigit(char c){return c>='0'&&c<='9';}
static const double W_TWO_PI = 6.283185307179586;

/* ===================== radix-2 512 FFT ================================ */
#define NFFT 512
typedef struct { float cs[NFFT/2]; float sn[NFFT/2]; } fft_tw_t;
static void fft_tw_init(fft_tw_t *w){
    int i; for(i=0;i<NFFT/2;i++){ float s,c; w_sincos(W_TWO_PI*(double)i/(double)NFFT,&s,&c); w->cs[i]=c; w->sn[i]=-s; }
}
/* in: ar/ai length NFFT (overwritten). forward DFT (sign -1). */
static void fft512(float *ar, float *ai, const fft_tw_t *w){
    int i,j,h,step,i0,s;
    for(i=0,j=0;i<NFFT;i++){
        if(j>i){ float t; t=ar[i];ar[i]=ar[j];ar[j]=t; t=ai[i];ai[i]=ai[j];ai[j]=t; }
        int bit=NFFT>>1; for(;j&bit;bit>>=1)j^=bit; j^=bit;
    }
    for(h=1;h<NFFT;h<<=1){
        step=NFFT/(2*h);
        for(i0=0;i0<NFFT;i0+=2*h){
            for(s=0;s<h;s++){
                int k=step*s; float cr=w->cs[k], ci=w->sn[k];
                float *pa_r=ar+i0+s,*pa_i=ai+i0+s,*pb_r=ar+i0+s+h,*pb_i=ai+i0+s+h;
                float tr=(*pb_r)*cr-(*pb_i)*ci, ti=(*pb_r)*ci+(*pb_i)*cr;
                *pb_r=*pa_r-tr; *pb_i=*pa_i-ti; *pa_r=*pa_r+tr; *pa_i=*pa_i+ti;
            }
        }
    }
}

/* ===================== metric table (WSJT-X Es/No=6 dB row) =========== */
static const float wspr_mtab2[256]={
    0.9999,0.9998,0.9998,0.9998,0.9998,0.9998,0.9997,0.9997,
    0.9997,0.9997,0.9997,0.9996,0.9996,0.9996,0.9995,0.9995,
    0.9994,0.9994,0.9994,0.9993,0.9993,0.9992,0.9991,0.9991,
    0.9990,0.9989,0.9988,0.9988,0.9988,0.9986,0.9985,0.9984,
    0.9983,0.9982,0.9980,0.9979,0.9977,0.9976,0.9974,0.9971,
    0.9969,0.9968,0.9965,0.9962,0.9960,0.9957,0.9953,0.9950,
    0.9947,0.9941,0.9937,0.9933,0.9928,0.9922,0.9917,0.9911,
    0.9904,0.9897,0.9890,0.9882,0.9874,0.9863,0.9855,0.9843,
    0.9832,0.9819,0.9806,0.9792,0.9777,0.9760,0.9743,0.9724,
    0.9704,0.9683,0.9659,0.9634,0.9609,0.9581,0.9550,0.9516,
    0.9481,0.9446,0.9406,0.9363,0.9317,0.9270,0.9218,0.9160,
    0.9103,0.9038,0.8972,0.8898,0.8822,0.8739,0.8647,0.8554,
    0.8457,0.8357,0.8231,0.8115,0.7984,0.7854,0.7704,0.7556,
    0.7391,0.7210,0.7038,0.6840,0.6633,0.6408,0.6174,0.5939,
    0.5678,0.5410,0.5137,0.4836,0.4524,0.4193,0.3850,0.3482,
    0.3132,0.2733,0.2315,0.1891,0.1435,0.0980,0.0493,0.0000,
    -0.0510,-0.1052,-0.1593,-0.2177,-0.2759,-0.3374,-0.4005,-0.4599,
    -0.5266,-0.5935,-0.6626,-0.7328,-0.8051,-0.8757,-0.9498,-1.0271,
    -1.1019,-1.1816,-1.2642,-1.3459,-1.4295,-1.5077,-1.5958,-1.6818,
    -1.7647,-1.8548,-1.9387,-2.0295,-2.1152,-2.2154,-2.3011,-2.3904,
    -2.4820,-2.5786,-2.6730,-2.7652,-2.8616,-2.9546,-3.0526,-3.1445,
    -3.2445,-3.3416,-3.4357,-3.5325,-3.6324,-3.7313,-3.8225,-3.9209,
    -4.0248,-4.1278,-4.2261,-4.3193,-4.4220,-4.5262,-4.6214,-4.7242,
    -4.8234,-4.9245,-5.0298,-5.1250,-5.2232,-5.3267,-5.4332,-5.5342,
    -5.6431,-5.7270,-5.8401,-5.9350,-6.0407,-6.1418,-6.2363,-6.3384,
    -6.4536,-6.5429,-6.6582,-6.7433,-6.8438,-6.9478,-7.0789,-7.1894,
    -7.2714,-7.3815,-7.4810,-7.5575,-7.6852,-7.8071,-7.8580,-7.9724,
    -8.1000,-8.2207,-8.2867,-8.4017,-8.5287,-8.6347,-8.7082,-8.8319,
    -8.9448,-9.0355,-9.1885,-9.2095,-9.2863,-9.4186,-9.5064,-9.6386,
    -9.7207,-9.8286,-9.9453,-10.0701,-10.1735,-10.3001,-10.2858,-10.5427,
    -10.5982,-10.7361,-10.7042,-10.9212,-11.0097,-11.0469,-11.1155,-11.2812,
    -11.3472,-11.4988,-11.5327,-11.6692,-11.9376,-11.8606,-12.1372,-13.2539
};

/* sync vector pr3 */
static const uint8_t pr3[162]={
    1,1,0,0,0,0,0,0,1,0,0,0,1,1,1,0,0,0,1,0,
    0,1,0,1,1,1,1,0,0,0,0,0,0,0,1,0,0,1,0,1,
    0,0,0,0,0,0,1,0,1,1,0,0,1,1,0,1,0,0,0,1,
    1,0,1,0,0,0,0,1,1,0,1,0,1,0,1,0,1,0,0,1,
    0,0,1,0,1,1,0,0,0,1,1,0,1,0,1,0,0,0,1,0,
    0,0,0,0,1,0,0,1,0,0,1,1,1,0,1,1,0,0,1,1,
    0,1,0,0,0,1,1,1,0,0,0,0,0,1,0,1,0,0,1,1,
    0,0,0,0,0,0,0,1,1,0,1,0,1,1,0,0,0,1,1,0,
    0,0};

/* ===================== Fano decoder =================================== */
#define WSPR_POLY1 0xF2D05351u
#define WSPR_POLY2 0xE4613C47u
#define FANO_NBITS 81
typedef struct { uint32_t encstate; int32_t gamma; int32_t metrics[4]; int32_t tm[2]; int i; } fnode_t;

static uint32_t enc_sym(uint32_t st){
    return (w_parity(st&WSPR_POLY1)<<1)|w_parity(st&WSPR_POLY2);
}
/* returns 0 ok, -1 timeout. symbols: 162 soft (0..255). dat: 10+ bytes. */
static int fano(fnode_t *nodes, const uint8_t *symbols, int mettab[2][256],
                int delta, unsigned maxcycles, uint8_t *dat, unsigned *metric_out){
    fnode_t *np, *lastnode=&nodes[FANO_NBITS-1], *tail=&nodes[FANO_NBITS-31];
    int t,m0,m1,ngamma; uint32_t lsym; unsigned i; const uint8_t *sp=symbols;
    for(np=nodes;np<=lastnode;np++){
        np->metrics[0]=mettab[0][sp[0]]+mettab[0][sp[1]];
        np->metrics[1]=mettab[0][sp[0]]+mettab[1][sp[1]];
        np->metrics[2]=mettab[1][sp[0]]+mettab[0][sp[1]];
        np->metrics[3]=mettab[1][sp[0]]+mettab[1][sp[1]];
        sp+=2;
    }
    np=nodes; np->encstate=0;
    lsym=enc_sym(np->encstate); m0=np->metrics[lsym]; m1=np->metrics[3^lsym];
    if(m0>m1){np->tm[0]=m0;np->tm[1]=m1;}else{np->tm[0]=m1;np->tm[1]=m0;np->encstate++;}
    np->i=0; maxcycles*=FANO_NBITS; np->gamma=t=0;
    for(i=1;i<=maxcycles;i++){
        ngamma=np->gamma+np->tm[np->i];
        if(ngamma>=t){
            if(np->gamma<t+delta){ while(ngamma>=t+delta)t+=delta; }
            np[1].gamma=ngamma; np[1].encstate=np->encstate<<1;
            if(++np==(lastnode+1)) break;
            lsym=enc_sym(np->encstate);
            if(np>=tail){ np->tm[0]=np->metrics[lsym]; }
            else{
                m0=np->metrics[lsym]; m1=np->metrics[3^lsym];
                if(m0>m1){np->tm[0]=m0;np->tm[1]=m1;}else{np->tm[0]=m1;np->tm[1]=m0;np->encstate++;}
            }
            np->i=0; continue;
        }
        for(;;){
            if(np==nodes||np[-1].gamma<t){
                t-=delta; if(np->i!=0){np->i=0;np->encstate^=1;} break;
            }
            if(--np<tail&&np->i!=1){ np->i++; np->encstate^=1; break; }
        }
    }
    *metric_out=(unsigned)np->gamma;
    { int nb=FANO_NBITS>>3; np=&nodes[7]; while(nb-->0){*dat++=(uint8_t)np->encstate; np+=8;} }
    return (i>=maxcycles)?-1:0;
}

/* ===================== interleave / unpack ============================ */
static void deinterleave(uint8_t *sym){
    uint8_t tmp[162]; unsigned p=0,i=0,j;
    while(p<162){ j=(uint8_t)(((i*0x80200802ULL)&0x0884422110ULL)*0x0101010101ULL>>32);
        if(j<162){tmp[p]=sym[j];p++;} i++; }
    for(i=0;i<162;i++)sym[i]=tmp[i];
}
static const char C37[]={'0','1','2','3','4','5','6','7','8','9','A','B','C','D','E',
    'F','G','H','I','J','K','L','M','N','O','P','Q','R','S','T','U','V','W','X','Y','Z',' '};
static void unpack50(const signed char *dat,int32_t *n1,int32_t *n2){
    int32_t i4;
    i4=dat[0]&255; *n1=i4<<20;
    i4=dat[1]&255; *n1+=i4<<12;
    i4=dat[2]&255; *n1+=i4<<4;
    i4=dat[3]&255; *n1+=(i4>>4)&15; *n2=(int32_t)(i4&15)<<18;
    i4=dat[4]&255; *n2+=i4<<10;
    i4=dat[5]&255; *n2+=i4<<2;
    i4=dat[6]&255; *n2+=(i4>>6)&3;
}
static int unpackcall(int32_t ncall,char *call){
    int32_t n=ncall; int i; char tmp[7];
    w_strcpy(call,"......");
    if(n<262177560){
        i=n%27+10; tmp[5]=C37[i]; n/=27;
        i=n%27+10; tmp[4]=C37[i]; n/=27;
        i=n%27+10; tmp[3]=C37[i]; n/=27;
        i=n%10;    tmp[2]=C37[i]; n/=10;
        i=n%36;    tmp[1]=C37[i]; n/=36;
        i=n;       tmp[0]=C37[i]; tmp[6]=0;
        for(i=0;i<5;i++) if(tmp[i]!=' ') break;
        { int j=0; while(tmp[i]) call[j++]=tmp[i++]; while(j<6)call[j++]=' '; call[6]=0; }
        for(i=0;i<6;i++) if(call[i]==' ') call[i]=0;
    } else return 0;
    return 1;
}
static int unpackgrid(int32_t ngrid,char *grid){
    int dlat,dlong; ngrid>>=7;
    if(ngrid<32400){
        dlat=(ngrid%180)-90; dlong=(ngrid/180)*2-180+2;
        if(dlong<-180){dlong+=360;}
        if(dlong>180){dlong+=360;}
        { int nlong=(int)(60.0*(180.0-dlong)/5.0); int n1=nlong/240; int n2=(nlong-240*n1)/24;
          grid[0]=C37[10+n1]; grid[2]=C37[n2];
          int nlat=(int)(60.0*(dlat+90)/2.5); n1=nlat/240; n2=(nlat-240*n1)/24;
          grid[1]=C37[10+n1]; grid[3]=C37[n2]; grid[4]=0; }
    } else { w_strcpy(grid,"XXXX"); return 0; }
    return 1;
}
static int unpackpfx(int32_t nprefix,char *call){
    char nc,pfx[4]={0,0,0,0},tmpcall[7]; int i; int32_t n;
    w_strcpy(tmpcall,call);
    if(nprefix<60000){
        n=nprefix;
        for(i=2;i>=0;i--){ nc=n%37;
            if(nc<=9)pfx[i]=nc+48; else if(nc<=35)pfx[i]=nc+55; else pfx[i]=' '; n/=37; }
        { int s=0; while(s<3&&pfx[s]==' ')s++; /* strip leading spaces */
          int j=0; while(s<3&&pfx[s])call[j++]=pfx[s++]; call[j++]='/';
          { const char*t=tmpcall; while(*t)call[j++]=*t++; } call[j]=0; }
    } else {
        nc=nprefix-60000;
        if(nc<=9){ pfx[0]=nc+48; w_strcpy(call,tmpcall); i=w_strlen(call); call[i]='/'; call[i+1]=pfx[0]; call[i+2]=0; }
        else if(nc<=35){ pfx[0]=nc+55; w_strcpy(call,tmpcall); i=w_strlen(call); call[i]='/'; call[i+1]=pfx[0]; call[i+2]=0; }
        else if(nc<=125){ pfx[0]=(nc-26)/10+48; pfx[1]=(nc-26)%10+48; w_strcpy(call,tmpcall);
            i=w_strlen(call); call[i]='/'; call[i+1]=pfx[0]; call[i+2]=pfx[1]; call[i+3]=0; }
        else return 0;
    }
    return 1;
}

/* small per-period hash table for type-2/3 call resolution */
#define HT_CAP 64
typedef struct { uint16_t used; uint16_t ih[HT_CAP]; char call[HT_CAP][13]; char grid[HT_CAP][5]; } htab_t;
static void ht_put(htab_t*h,int ih,const char*call,const char*grid){
    int i; for(i=0;i<h->used;i++) if(h->ih[i]==ih){ w_strcpy(h->call[i],call); if(grid)w_strcpy(h->grid[i],grid); return; }
    if(h->used<HT_CAP){ h->ih[h->used]=(uint16_t)ih; w_strcpy(h->call[h->used],call);
        if(grid)w_strcpy(h->grid[h->used],grid); else h->grid[h->used][0]=0; h->used++; }
}
static const char* ht_getcall(htab_t*h,int ih){ int i; for(i=0;i<h->used;i++) if(h->ih[i]==ih) return h->call[i]; return 0; }

/* Jenkins lookup3 (nhash), initval 146, masked to 15 bits */
#define W_ROT(x,k) (((x)<<(k))|((x)>>(32-(k))))
static uint32_t le32(const unsigned char*p,int n){uint32_t v=0;int i;for(i=0;i<n;i++)v|=(uint32_t)p[i]<<(8*i);return v;}
static int nhash(const char*key,int length){
    uint32_t a,b,c; const unsigned char*k=(const unsigned char*)key;
    a=b=c=0xdeadbeef+(uint32_t)length+146u;
    while(length>12){ a+=le32(k,4);b+=le32(k+4,4);c+=le32(k+8,4);
        a-=c;a^=W_ROT(c,4);c+=b; b-=a;b^=W_ROT(a,6);a+=c; c-=b;c^=W_ROT(b,8);b+=a;
        a-=c;a^=W_ROT(c,16);c+=b; b-=a;b^=W_ROT(a,19);a+=c; c-=b;c^=W_ROT(b,4);b+=a;
        length-=12;k+=12; }
    switch(length){
    case 12:c+=le32(k+8,4);b+=le32(k+4,4);a+=le32(k,4);break;
    case 11:c+=le32(k+8,3);b+=le32(k+4,4);a+=le32(k,4);break;
    case 10:c+=le32(k+8,2);b+=le32(k+4,4);a+=le32(k,4);break;
    case 9: c+=le32(k+8,1);b+=le32(k+4,4);a+=le32(k,4);break;
    case 8: b+=le32(k+4,4);a+=le32(k,4);break;
    case 7: b+=le32(k+4,3);a+=le32(k,4);break;
    case 6: b+=le32(k+4,2);a+=le32(k,4);break;
    case 5: b+=le32(k+4,1);a+=le32(k,4);break;
    case 4: a+=le32(k,4);break;
    case 3: a+=le32(k,3);break;
    case 2: a+=le32(k,2);break;
    case 1: a+=le32(k,1);break;
    case 0: return (int)(c&32767u); }
    c^=b;c-=W_ROT(b,14); a^=c;a-=W_ROT(c,11); b^=a;b-=W_ROT(a,25);
    c^=b;c-=W_ROT(b,16); a^=c;a-=W_ROT(c,4);  b^=a;b-=W_ROT(a,14); c^=b;c-=W_ROT(b,24);
    return (int)(c&32767u);
}

/* int -> "%2d" power string */
static void dbm_str(int v,char*out){
    if(v<0){out[0]='-';v=-v;out[1]=(char)('0'+v%10);out[2]=0;return;}
    if(v>=10){out[0]=(char)('0'+v/10);out[1]=(char)('0'+v%10);out[2]=0;}
    else{out[0]=' ';out[1]=(char)('0'+v);out[2]=0;}
}
static int w_atoin(const char*s){int v=0,sg=1;if(*s=='-'){sg=-1;s++;}while(w_isdigit(*s)){v=v*10+(*s-'0');s++;}return sg*v;}

/* unpk_: returns noprint flag; fills call_loc_pow, callsign, and dbm_out. */
static int unpk_(const signed char *message, htab_t *ht, char *clp, char *callsign, int *dbm_out){
    int32_t n1,n2,n3; int ndbm,ihash,nadd,noprint=0; char grid[5],grid6[7],cdbm[4];
    unpack50(message,&n1,&n2);
    if(!unpackcall(n1,callsign)) return 1;
    if(!unpackgrid(n2,grid)) return 1;
    int ntype=(n2&127)-64; grid[4]=0;
    if(ntype>=0&&ntype<=62){
        int nu=ntype%10;
        if(nu==0||nu==3||nu==7){
            ndbm=ntype; clp[0]=0; dbm_str(ndbm,cdbm);
            { char*d=clp; const char*s=callsign; while(*s)*d++=*s++; *d++=' ';
              int i; for(i=0;i<4;i++)*d++=grid[i]; *d++=' '; *d++=cdbm[0]; *d++=cdbm[1]; *d=0; }
            ihash=nhash(callsign,w_strlen(callsign)); ht_put(ht,ihash,callsign,grid);
        } else {
            nadd=nu; if(nu>3)nadd=nu-3; if(nu>7)nadd=nu-7;
            n3=n2/128+32768*(nadd-1);
            if(!unpackpfx(n3,callsign)) return 1;
            ndbm=ntype-nadd; clp[0]=0; dbm_str(ndbm,cdbm);
            { char*d=clp; const char*s=callsign; while(*s)*d++=*s++; *d++=' '; *d++=cdbm[0]; *d++=cdbm[1]; *d=0; }
            nu=ndbm%10;
            if(nu==0||nu==3||nu==7||nu==10){ ihash=nhash(callsign,w_strlen(callsign)); ht_put(ht,ihash,callsign,0); }
            else noprint=1;
        }
    } else { /* ntype<0 : type 3, hashed call + 6-char grid */
        ndbm=-(ntype+1);
        { int len=6,i,j=0; grid6[0]=0;
          /* grid6 = callsign[len-1] + callsign[0..len-2] (callsign holds packed 6-char grid) */
          grid6[j++]=callsign[len-1]; for(i=0;i<len-1;i++)grid6[j++]=callsign[i]; grid6[j]=0; }
        int nu=ndbm%10;
        if((nu!=0&&nu!=3&&nu!=7&&nu!=10)||!w_isalpha(grid6[0])||!w_isalpha(grid6[1])||!w_isdigit(grid6[2])||!w_isdigit(grid6[3])) noprint=1;
        ihash=(n2-ntype-64)/128;
        { const char*hc=ht_getcall(ht,ihash);
          if(hc){ int j=1; const char*s=hc; callsign[0]='<'; while(*s)callsign[j++]=*s++; callsign[j++]='>'; callsign[j]=0; }
          else w_strcpy(callsign,"<...>"); }
        dbm_str(ndbm,cdbm); clp[0]=0;
        { char*d=clp; const char*s=callsign; while(*s)*d++=*s++; *d++=' '; const char*g=grid6; while(*g)*d++=*g++; *d++=' '; *d++=cdbm[0]; *d++=cdbm[1]; *d=0; }
        if(ntype==-64) noprint=1;
    }
    *dbm_out=ndbm;
    return noprint;
}

/* ===================== context / work arena ==========================
 * Reduced-memory layout to fit the IC-705 on-chip region (<=240 KiB):
 *  - idat/qdat stored as s16 (scale-invariant decode; the analysis measured
 *    identical decode counts to float at every SNR) -> 2 x 88 KiB.
 *  - the full 512xNFFT power spectrogram is NOT stored: find_candidates only
 *    keeps psavg[512]; the coarse search recomputes a ~17-bin band per
 *    candidate (identical float values, +a few s compute).
 *  - subtraction streams a ~nfilt-sample ci/cq ring instead of full arrays.
 *  - the big per-call auto arrays of sync/noncoh/subtract live in W->scr
 *    (the work arena), keeping the task stack well under 16 KiB.                */
#define WSPR_SCR_FLOATS 6912u          /* shared phase-local scratch (27 KiB)   */
typedef struct { float freq,snr; int shift; float drift,sync; } cand_t;
typedef struct {
    int16_t *idat,*qdat;   /* 375 sps I/Q, s16 (scaled; decode is scale-free) */
    float  *scr;           /* phase-local scratch, WSPR_SCR_FLOATS floats      */
    int     npoints, nffts, ntw;        /* ntw = time windows kept in the band */
    cand_t *cand;          /* 200 */
    fnode_t *nodes;        /* 83 */
    /* small persistent buffers (kept out of the stack) */
    char    allcalls[100][13]; float allfreqs[100];
    uint8_t symbols[162], chan_sym[162], decdata[11]; signed char message11[11];
    float   wwin[512];     /* FFT analysis window (sin ramp), precomputed       */
    int     mettab[2][256];
    htab_t  ht;
    fft_tw_t tw;
} wctx_t;

/* ===================== sync_and_demodulate =========================== */
/* mode 0: find best lag; 1: find best freq; 2: soft symbols */
static void sync_and_demod(wctx_t*W,float*symf,float*f1,int ifmin,int ifmax,float fstep,
    int*shift1,int lagmin,int lagmax,int lagstep,float*drift1,int symfac,float*sync,int mode){
    static const float dt=1.0f/375.0f, df=375.0f/256.0f;
    float twopidt=(float)(W_TWO_PI*(double)dt), df15=df*1.5f, df05=df*0.5f;
    int i,j,k,lag,np=W->npoints; const int16_t *id=W->idat,*qd=W->qdat;
    /* big autos live in the work arena (scr), not on the stack */
    float *sp=W->scr;
    float *i0=sp,*q0=sp+162,*i1=sp+324,*q1=sp+486,*i2=sp+648,*q2=sp+810,*i3a=sp+972,*q3a=sp+1134;
    float *c0=sp+1296,*s0=sp+1552,*c1=sp+1808,*s1=sp+2064,*c2=sp+2320,*s2=sp+2576,*c3=sp+2832,*s3=sp+3088;
    float *fsymb=sp+3344;   /* uses scr[0..3505] */
    float p0,p1,p2,p3,cmet,totp,syncmax,fac;
    float f0=0.0f,fp,ss,fbest=0.0f,fsum=0.0f,f2sum=0.0f;
    int best_shift=0,ifreq; float fplast=-10000.0f;
    syncmax=-1e30f;
    if(mode==0){ifmin=0;ifmax=0;fstep=0.0f;f0=*f1;}
    if(mode==1){lagmin=*shift1;lagmax=*shift1;f0=*f1;}
    if(mode==2){lagmin=*shift1;lagmax=*shift1;ifmin=0;ifmax=0;f0=*f1;}
    for(ifreq=ifmin;ifreq<=ifmax;ifreq++){
        f0=*f1+ifreq*fstep;
        for(lag=lagmin;lag<=lagmax;lag+=lagstep){
            ss=0.0f; totp=0.0f;
            for(i=0;i<162;i++){
                fp=f0+(*drift1/2.0f)*((float)i-81.0f)/81.0f;
                if(i==0||fp!=fplast){
                    float cd,sd;
                    w_sincos((double)(twopidt*(fp-df15)),&sd,&cd); c0[0]=1;s0[0]=0;
                    { float cc=cd,ssn=sd; for(j=1;j<256;j++){c0[j]=c0[j-1]*cc-s0[j-1]*ssn;s0[j]=c0[j-1]*ssn+s0[j-1]*cc;} }
                    w_sincos((double)(twopidt*(fp-df05)),&sd,&cd); c1[0]=1;s1[0]=0;
                    { float cc=cd,ssn=sd; for(j=1;j<256;j++){c1[j]=c1[j-1]*cc-s1[j-1]*ssn;s1[j]=c1[j-1]*ssn+s1[j-1]*cc;} }
                    w_sincos((double)(twopidt*(fp+df05)),&sd,&cd); c2[0]=1;s2[0]=0;
                    { float cc=cd,ssn=sd; for(j=1;j<256;j++){c2[j]=c2[j-1]*cc-s2[j-1]*ssn;s2[j]=c2[j-1]*ssn+s2[j-1]*cc;} }
                    w_sincos((double)(twopidt*(fp+df15)),&sd,&cd); c3[0]=1;s3[0]=0;
                    { float cc=cd,ssn=sd; for(j=1;j<256;j++){c3[j]=c3[j-1]*cc-s3[j-1]*ssn;s3[j]=c3[j-1]*ssn+s3[j-1]*cc;} }
                    fplast=fp;
                }
                i0[i]=q0[i]=i1[i]=q1[i]=i2[i]=q2[i]=i3a[i]=q3a[i]=0.0f;
                for(j=0;j<256;j++){ k=lag+i*256+j;
                    if(k>0&&k<np){ float idk=(float)id[k],qdk=(float)qd[k];
                        i0[i]+=idk*c0[j]+qdk*s0[j]; q0[i]+=-idk*s0[j]+qdk*c0[j];
                        i1[i]+=idk*c1[j]+qdk*s1[j]; q1[i]+=-idk*s1[j]+qdk*c1[j];
                        i2[i]+=idk*c2[j]+qdk*s2[j]; q2[i]+=-idk*s2[j]+qdk*c2[j];
                        i3a[i]+=idk*c3[j]+qdk*s3[j]; q3a[i]+=-idk*s3[j]+qdk*c3[j]; } }
                p0=w_sqrtf(i0[i]*i0[i]+q0[i]*q0[i]); p1=w_sqrtf(i1[i]*i1[i]+q1[i]*q1[i]);
                p2=w_sqrtf(i2[i]*i2[i]+q2[i]*q2[i]); p3=w_sqrtf(i3a[i]*i3a[i]+q3a[i]*q3a[i]);
                totp+=p0+p1+p2+p3; cmet=(p1+p3)-(p0+p2);
                ss=(pr3[i]==1)?ss+cmet:ss-cmet;
                if(mode==2){ fsymb[i]=(pr3[i]==1)?(p3-p1):(p2-p0); }
            }
            ss=ss/totp;
            if(ss>syncmax){syncmax=ss;best_shift=lag;fbest=f0;}
        }
    }
    if(mode<=1){*sync=syncmax;*shift1=best_shift;*f1=fbest;return;}
    if(mode==2){ *sync=syncmax;
        for(i=0;i<162;i++){fsum+=fsymb[i]/162.0f;f2sum+=fsymb[i]*fsymb[i]/162.0f;}
        fac=w_sqrtf(f2sum-fsum*fsum);
        for(i=0;i<162;i++){ float v=symfac*fsymb[i]/fac; if(v>127)v=127; if(v<-128)v=-128; symf[i]=v+128.0f; }
    }
}

/* ===================== noncoherent_sequence_detection ================= */
static void noncoh_detect(wctx_t*W,uint8_t*symbols,float*f1,int*shift1,float*drift1,
    int symfac,int nblock,int bitbybit){
    static const float dt=1.0f/375.0f, df=375.0f/256.0f;
    float twopidt=(float)(W_TWO_PI*(double)dt), df15=df*1.5f, df05=df*0.5f;
    int i,j,k,lag,itone,ib,b,nseq,imask,np=W->npoints; const int16_t *id=W->idat,*qd=W->qdat;
    /* all big autos live in the work arena (scr), not on the stack */
    float *sp=W->scr;
    float (*is)[162]=(float(*)[162])(sp+0),   (*qs)[162]=(float(*)[162])(sp+648);
    float (*cf)[162]=(float(*)[162])(sp+1296),(*sf)[162]=(float(*)[162])(sp+1944);
    float *c0=sp+2592,*s0=sp+2849,*c1=sp+3106,*s1=sp+3363,*c2=sp+3620,*s2=sp+3877,*c3=sp+4134,*s3=sp+4391;
    float *xi=sp+4648,*xq=sp+5160,*p=sp+5672,*fsymb=sp+6184;   /* uses scr[0..6345] */
    float cm,sm,cmp,smp,fac,xm1,xm0;
    float f0,fp,fsum=0.0f,f2sum=0.0f,fplast=-10000.0f;
    f0=*f1; lag=*shift1; nseq=1<<nblock;
    for(i=0;i<162;i++){
        fp=f0+(*drift1/2.0f)*((float)i-81.0f)/81.0f;
        if(i==0||fp!=fplast){ float cd,sd;
            w_sincos((double)(twopidt*(fp-df15)),&sd,&cd);c0[0]=1;s0[0]=0;{float cc=cd,sn=sd;for(j=1;j<257;j++){c0[j]=c0[j-1]*cc-s0[j-1]*sn;s0[j]=c0[j-1]*sn+s0[j-1]*cc;}}
            w_sincos((double)(twopidt*(fp-df05)),&sd,&cd);c1[0]=1;s1[0]=0;{float cc=cd,sn=sd;for(j=1;j<257;j++){c1[j]=c1[j-1]*cc-s1[j-1]*sn;s1[j]=c1[j-1]*sn+s1[j-1]*cc;}}
            w_sincos((double)(twopidt*(fp+df05)),&sd,&cd);c2[0]=1;s2[0]=0;{float cc=cd,sn=sd;for(j=1;j<257;j++){c2[j]=c2[j-1]*cc-s2[j-1]*sn;s2[j]=c2[j-1]*sn+s2[j-1]*cc;}}
            w_sincos((double)(twopidt*(fp+df15)),&sd,&cd);c3[0]=1;s3[0]=0;{float cc=cd,sn=sd;for(j=1;j<257;j++){c3[j]=c3[j-1]*cc-s3[j-1]*sn;s3[j]=c3[j-1]*sn+s3[j-1]*cc;}}
            fplast=fp;
        }
        cf[0][i]=c0[256];sf[0][i]=s0[256]; cf[1][i]=c1[256];sf[1][i]=s1[256];
        cf[2][i]=c2[256];sf[2][i]=s2[256]; cf[3][i]=c3[256];sf[3][i]=s3[256];
        is[0][i]=qs[0][i]=is[1][i]=qs[1][i]=is[2][i]=qs[2][i]=is[3][i]=qs[3][i]=0.0f;
        for(j=0;j<256;j++){ k=lag+i*256+j;
            if(k>0&&k<np){ float idk=(float)id[k],qdk=(float)qd[k];
                is[0][i]+=idk*c0[j]+qdk*s0[j];qs[0][i]+=-idk*s0[j]+qdk*c0[j];
                is[1][i]+=idk*c1[j]+qdk*s1[j];qs[1][i]+=-idk*s1[j]+qdk*c1[j];
                is[2][i]+=idk*c2[j]+qdk*s2[j];qs[2][i]+=-idk*s2[j]+qdk*c2[j];
                is[3][i]+=idk*c3[j]+qdk*s3[j];qs[3][i]+=-idk*s3[j]+qdk*c3[j]; } }
    }
    for(i=0;i<162;i+=nblock){
        for(j=0;j<nseq;j++){
            xi[j]=0.0f;xq[j]=0.0f;cm=1;sm=0;
            for(ib=0;ib<nblock;ib++){
                b=(j&(1<<(nblock-1-ib)))>>(nblock-1-ib);
                itone=pr3[i+ib]+2*b;
                xi[j]+=is[itone][i+ib]*cm+qs[itone][i+ib]*sm;
                xq[j]+=qs[itone][i+ib]*cm-is[itone][i+ib]*sm;
                cmp=cf[itone][i+ib]*cm-sf[itone][i+ib]*sm; smp=sf[itone][i+ib]*cm+cf[itone][i+ib]*sm; cm=cmp;sm=smp;
            }
            p[j]=w_sqrtf(xi[j]*xi[j]+xq[j]*xq[j]);
        }
        for(ib=0;ib<nblock;ib++){ imask=1<<(nblock-1-ib); xm1=0;xm0=0;
            for(j=0;j<nseq;j++){ if((j&imask)!=0){if(p[j]>xm1)xm1=p[j];} if((j&imask)==0){if(p[j]>xm0)xm0=p[j];} }
            fsymb[i+ib]=xm1-xm0;
            if(bitbybit)fsymb[i+ib]=fsymb[i+ib]/(xm1>xm0?xm1:xm0);
        }
    }
    for(i=0;i<162;i++){fsum+=fsymb[i]/162.0f;f2sum+=fsymb[i]*fsymb[i]/162.0f;}
    fac=w_sqrtf(f2sum-fsum*fsum);
    for(i=0;i<162;i++){ float v=symfac*fsymb[i]/fac; if(v>127)v=127;if(v<-128)v=-128; symbols[i]=(uint8_t)(int)(v+128.0f); }
}

/* ===================== subtract_signal2 (streaming) ================== */
/* wsprd: c(t) = LPF[s(t) conj(r(t))] with a CENTRED nfilt-point window (c at sample i uses s*conj(r) of samples
 * i-nfilt/2 .. i+nfilt/2-1), then s'(t) = s(t) - c(t) r(t).  Here in a single forward pass: the product d[t] runs
 * nfilt/2 samples ahead of the sample being subtracted, a 2*nfilt sliding ci/cq window holds the last nfilt
 * products and a nfilt/2 delay line holds the reference phase (~11 KiB of scr instead of six 45000-float arrays).
 * d[t] is always taken from id/qd BEFORE that sample is subtracted (the read index is ahead of the write index),
 * i.e. the same values as wsprd's two-pass version.
 * ab (may be NULL) is called every 4096 samples as a keep-alive only: its result is ignored here, a half-subtracted
 * signal must not be left behind (the caller polls again right after). */
static int16_t clamp16(float v){ return v>32767.0f?32767:(v<-32768.0f?-32768:(int16_t)(v>=0?v+0.5f:v-0.5f)); }
static void subtract_signal2(wctx_t*W,float f0,int shift0,float drift0,const uint8_t*chan,int(*ab)(void)){
    static const float dt=1.0f/375.0f, df=375.0f/256.0f;
    float twopidt=(float)(W_TWO_PI*(double)dt);
    int i,k,t,nsym=162,nspersym=256,nfilt=360,H=nfilt/2,nsig=nsym*nspersym,np=W->npoints;
    int16_t *id=W->idat,*qd=W->qdat;
    float *sp=W->scr;
    float *ci=sp, *cq=sp+2*nfilt, *w=sp+4*nfilt, *partialsum=sp+5*nfilt;
    float *dlc=sp+6*nfilt, *dls=sp+6*nfilt+H;         /* reference cos/sin of the last H samples; 7*nfilt=2520 floats */
    float norm=0.0f;
    for(i=0;i<nfilt;i++){ w[i]=w_sinf((double)(3.14159265358979f*(float)i/(float)(nfilt-1))); norm+=w[i]; }
    for(i=0;i<nfilt;i++) w[i]/=norm;
    partialsum[0]=0.0f; for(i=1;i<nfilt;i++)partialsum[i]=partialsum[i-1]+w[i];
    for(i=0;i<2*nfilt;i++){ci[i]=0.0f;cq[i]=0.0f;}   /* leading nfilt = zero pad */
    for(i=0;i<H;i++){dlc[i]=0.0f;dls[i]=0.0f;}
    int pos=nfilt;                                    /* next write index         */
    float phi=0.0f,dphi=0.0f;                         /* float like wsprd (same reference phase, same residual) */
    for(t=0;t<nsig+H;t++){
        float rc=0.0f,rs=0.0f; int slot=t%H;
        if(ab&&(t&4095)==4095)(void)ab();
        if(t<nsig){
            if((t&(nspersym-1))==0){ i=t/nspersym;
                dphi=(float)((double)twopidt*((double)f0+((double)drift0/2.0)*(double)((float)i-(float)nsym/2.0f)/(double)((float)nsym/2.0f)+((double)(float)chan[i]-1.5)*(double)df)); }
            w_sincos((double)phi,&rs,&rc); phi=phi+dphi;
        }
        if(t>=H){                                     /* subtract at sample io = t-H: window d[io-H .. io+H-1] */
            int io=t-H,base=pos-nfilt,jj; float cfi=0.0f,cfq=0.0f,rco=dlc[slot],rso=dls[slot];
            for(jj=0;jj<nfilt;jj++){ cfi+=w[jj]*ci[base+jj]; cfq+=w[jj]*cq[base+jj]; }
            if(io<nfilt/2) norm=partialsum[nfilt/2+io];
            else if(io>(nsig-1-nfilt/2)) norm=partialsum[nfilt/2+nsig-1-io];
            else norm=1.0f;
            k=shift0+io;
            if(k>0&&k<np){
                id[k]=clamp16((float)id[k]-(cfi*rco-cfq*rso)/norm);
                qd[k]=clamp16((float)qd[k]-(cfi*rso+cfq*rco)/norm);
            }
        }
        {   float dI=0.0f,dQ=0.0f;                    /* d[t] = s*conj(r), from the original id/qd */
            if(t<nsig){
                k=shift0+t;
                if(k>0&&k<np){ float oi=(float)id[k],oq=(float)qd[k]; dI=oi*rc+oq*rs; dQ=oq*rc-oi*rs; }
                dlc[slot]=rc; dls[slot]=rs;
            }
            ci[pos]=dI; cq[pos]=dQ; pos++;
            if(pos==2*nfilt){ int u; for(u=0;u<nfilt;u++){ci[u]=ci[nfilt+u];cq[u]=cq[nfilt+u];} pos=nfilt; }
        }
    }
}

/* windowed FFT of time-window i of idat/qdat into fr/fi, fftshifted (bin 256=DC).
 * Shift = swap of the two 256-point halves (done in place, no stack temp). */
static void fft_window(wctx_t*W,int i,float*fr,float*fi){
    const int16_t *id=W->idat,*qd=W->qdat; const float*wwin=W->wwin; int j,k;
    for(j=0;j<512;j++){ k=i*128+j; fr[j]=(float)id[k]*wwin[j]; fi[j]=(float)qd[k]*wwin[j]; }
    fft512(fr,fi,&W->tw);
    for(j=0;j<256;j++){ float t; t=fr[j];fr[j]=fr[j+256];fr[j+256]=t; t=fi[j];fi[j]=fi[j+256];fi[j+256]=t; }
}

/* ===================== candidate spectrum ============================ */
/* Only psavg is formed (no full spectrogram stored); the coarse search later
 * recomputes the per-window power of the ~17-bin band it needs per candidate. */
static int find_candidates(wctx_t*W,float fmin,float fmax,cand_t*cand,int*npk_out){
    int i,j,k,nffts=W->nffts;
    float *sp=W->scr;
    float *psavg=sp, *fr=sp+512, *fi=sp+1024, *smspec=sp+1536, *tmpsort=sp+1536+411;
    static const float df=375.0f/256.0f/2.0f;
    for(j=0;j<512;j++)psavg[j]=0.0f;
    for(i=0;i<nffts;i++){
        fft_window(W,i,fr,fi);
        for(j=0;j<512;j++){ float pw=fr[j]*fr[j]+fi[j]*fi[j]; psavg[j]+=pw; }
    }
    for(i=0;i<411;i++){ smspec[i]=0.0f; for(j=-3;j<=3;j++){ k=256-205+i+j; smspec[i]+=psavg[k]; } }
    for(j=0;j<411;j++)tmpsort[j]=smspec[j];
    /* insertion/selection sort (411 elements) ascending */
    for(i=0;i<411;i++){ int mi=i; for(j=i+1;j<411;j++)if(tmpsort[j]<tmpsort[mi])mi=j; float t=tmpsort[i];tmpsort[i]=tmpsort[mi];tmpsort[mi]=t; }
    float noise_level=tmpsort[122];
#ifdef WSPR_DEBUG
    { int pk=0; float mx=0; for(i=0;i<411;i++)if(smspec[i]>mx){mx=smspec[i];pk=i;}
      DBG("  smspec: noise=%.3g peakbin=%d peakf=%.2f peakval=%.3g ratio=%.1f snr=%.1f\n",
          noise_level,pk,(pk-205)*df,mx,mx/noise_level,10.0f*w_log10f(mx/noise_level-1.0f)-26.3f); }
#endif
    float min_snr=0.15848932f; /* 10^(-0.8) */ float snr_scale=26.3f;
    for(j=0;j<411;j++){ smspec[j]=smspec[j]/noise_level-1.0f; if(smspec[j]<min_snr)smspec[j]=0.1f*min_snr; }
    int npk=0;
    for(j=1;j<410;j++){
        if(smspec[j]>smspec[j-1]&&smspec[j]>smspec[j+1]&&npk<200){
            cand[npk].freq=(j-205)*df; cand[npk].snr=10.0f*w_log10f(smspec[j])-snr_scale;
            cand[npk].drift=0;cand[npk].shift=0;cand[npk].sync=0; npk++;
        }
    }
    /* restrict to [fmin,fmax] */
    i=0; for(j=0;j<npk;j++) if(cand[j].freq>=fmin&&cand[j].freq<=fmax){cand[i]=cand[j];i++;} npk=i;
    /* sort by snr desc (bubble) */
    { int pass2,kk; cand_t tmp; for(pass2=1;pass2<=npk-1;pass2++)for(kk=0;kk<npk-pass2;kk++)
        if(cand[kk].snr<cand[kk+1].snr){tmp=cand[kk];cand[kk]=cand[kk+1];cand[kk+1]=tmp;} }
    *npk_out=npk;
    return 0;
}

/* Recompute the per-window power of the 17-bin band centred on cbin (bins
 * cbin-8..cbin+8) across the first ntw time windows -> band[17*ntw], laid out
 * band[bi*ntw + t].  Identical float values to the once-stored spectrogram. */
static void compute_band(wctx_t*W,int cbin,float*band,float*fr,float*fi,int ntw){
    int t,bi,bin,lo=cbin-8;
    for(t=0;t<ntw;t++){
        fft_window(W,t,fr,fi);
        for(bi=0;bi<17;bi++){ bin=lo+bi;
            band[bi*ntw+t]=(bin>=0&&bin<512)?(fr[bin]*fr[bin]+fi[bin]*fi[bin]):0.0f; }
    }
}

/* ===================== main decode over I/Q ========================= */
static int decode_core(wctx_t*W,float fmin,float fmax,wspr_result_t*out,int maxout,int(*ab)(void)){
    static const float df=375.0f/256.0f/2.0f, dt=1.0f/375.0f;
    cand_t*cand=W->cand; int npk; int ipass,npasses=3; int ndecodes_pass=0;
    int symfac=50,delta=60; unsigned maxcycles=10000; float minrms=52.0f*(50.0f/64.0f);
    int uniques=0; int nuniq=0;
    /* arena-resident (kept off the stack) */
    float *allfreqs=W->allfreqs; char (*allcalls)[13]=W->allcalls;
    uint8_t *symbols=W->symbols, *decdata=W->decdata, *chan_sym=W->chan_sym;
    signed char *message11=W->message11;
    int i,j,k;
    for(i=0;i<100;i++)allfreqs[i]=0.0f;
    W->ht.used=0;
    for(ipass=0;ipass<npasses;ipass++){
        if(ab&&ab())return uniques?uniques:WSPR_ERR_ABORT;
        if(ipass==1&&ndecodes_pass==0&&npasses>2)ipass=2;
        int nblocksize,maxdrift; float minsync2;
        if(ipass<2){nblocksize=1;maxdrift=4;minsync2=0.12f;}
        else{nblocksize=4;maxdrift=0;minsync2=0.10f;}
        float minsync1=0.10f;
        ndecodes_pass=0;
        find_candidates(W,fmin,fmax,cand,&npk);
#ifdef WSPR_DEBUG
        { int d; for(d=0;d<npk&&d<16;d++) DBG("  det[%d] f=%.2f snr=%.0f\n",d,cand[d].freq,cand[d].snr); }
#endif
        /* coarse shift/freq/drift per candidate: recompute the 17-bin band */
        { int idrift,ifr,if0,ifd,k0,kindex,ntw=W->ntw;
          float *band=W->scr, *fr=W->scr+17*ntw, *fi=W->scr+17*ntw+512;
          float smax,ss,powr,p0,p1,p2,p3;
          for(j=0;j<npk;j++){ if(ab&&ab())return uniques?uniques:WSPR_ERR_ABORT;
            smax=-1e30f; if0=(int)(cand[j].freq/df)+256; int lo=if0-8;
            compute_band(W,if0,band,fr,fi,ntw);
            if(ab&&ab())return uniques?uniques:WSPR_ERR_ABORT;
            for(ifr=if0-2;ifr<=if0+2;ifr++) for(k0=-10;k0<22;k0++) for(idrift=-maxdrift;idrift<=maxdrift;idrift++){
                ss=0;powr=0;
                for(k=0;k<162;k++){ ifd=(int)((float)ifr+((float)k-81.0f)/81.0f*((float)idrift)/(2.0f*df)); kindex=k0+2*k;
                    if(kindex>=0&&kindex<ntw&&ifd-3>=0&&ifd+3<512){
                        int b0=ifd-3-lo,b3=ifd+3-lo;
                        if(b0>=0&&b3<17){
                            p0=w_sqrtf(band[b0*ntw+kindex]);p1=w_sqrtf(band[(ifd-1-lo)*ntw+kindex]);
                            p2=w_sqrtf(band[(ifd+1-lo)*ntw+kindex]);p3=w_sqrtf(band[b3*ntw+kindex]);
                            ss+=(2*pr3[k]-1)*((p1+p3)-(p0+p2)); powr+=p0+p1+p2+p3; } } }
                if(powr>0){ float sync1=ss/powr; if(sync1>smax){smax=sync1;cand[j].shift=128*(k0+1);cand[j].drift=idrift;cand[j].freq=(ifr-256)*df;cand[j].sync=sync1;} }
            }
            DBG("  cand[%d] coarse-> f=%.2f sh=%d dr=%.0f sync=%.3f (detsnr=%.0f)\n",j,cand[j].freq,cand[j].shift,cand[j].drift,cand[j].sync,cand[j].snr);
          }
        }
        /* fine sync per candidate */
        for(j=0;j<npk;j++){
            if(ab&&ab())return uniques?uniques:WSPR_ERR_ABORT;
            float f1=cand[j].freq,drift1=cand[j].drift,sync1=cand[j].sync; int shift1=cand[j].shift;
            int ifmin,ifmax; float fstep; int lagmin,lagmax,lagstep; static float symf[162];
            fstep=0;ifmin=0;ifmax=0; lagmin=shift1-128;lagmax=shift1+128;lagstep=64;
            sync_and_demod(W,symf,&f1,ifmin,ifmax,fstep,&shift1,lagmin,lagmax,lagstep,&drift1,symfac,&sync1,0);
            if(ab&&ab())return uniques?uniques:WSPR_ERR_ABORT;
            fstep=0.25f;ifmin=-2;ifmax=2;
            sync_and_demod(W,symf,&f1,ifmin,ifmax,fstep,&shift1,lagmin,lagmax,lagstep,&drift1,symfac,&sync1,1);
            if(ab&&ab())return uniques?uniques:WSPR_ERR_ABORT;
            if(ipass<2){ fstep=0;ifmin=0;ifmax=0; float driftp=drift1+0.5f,driftm=drift1-0.5f,syncp,syncm;
                sync_and_demod(W,symf,&f1,ifmin,ifmax,fstep,&shift1,lagmin,lagmax,lagstep,&driftp,symfac,&syncp,1);
                sync_and_demod(W,symf,&f1,ifmin,ifmax,fstep,&shift1,lagmin,lagmax,lagstep,&driftm,symfac,&syncm,1);
                if(syncp>sync1){drift1=driftp;sync1=syncp;} else if(syncm>sync1){drift1=driftm;sync1=syncm;} }
            if(sync1>minsync1){
                if(ab&&ab())return uniques?uniques:WSPR_ERR_ABORT;
                lagmin=shift1-32;lagmax=shift1+32;lagstep=16;
                sync_and_demod(W,symf,&f1,ifmin,ifmax,fstep,&shift1,lagmin,lagmax,lagstep,&drift1,symfac,&sync1,0);
                fstep=0.05f;ifmin=-2;ifmax=2;
                sync_and_demod(W,symf,&f1,ifmin,ifmax,fstep,&shift1,lagmin,lagmax,lagstep,&drift1,symfac,&sync1,1);
                cand[j].freq=f1;cand[j].shift=shift1;cand[j].drift=drift1;cand[j].sync=sync1;
            }
        }
        /* dedup -> nwat */
        int nwat=0,idupe;
        for(j=0;j<npk;j++){ idupe=0; for(k=0;k<nwat;k++){ float dfq=cand[j].freq-cand[k].freq; if(dfq<0)dfq=-dfq;
            int ds=cand[j].shift-cand[k].shift; if(ds<0)ds=-ds; if(dfq<0.05f&&ds<16){idupe=1;break;} }
            if(idupe){ if(cand[j].sync>cand[k].sync)cand[k]=cand[j]; }
            else if(cand[j].sync>minsync2){ cand[nwat]=cand[j]; nwat++; } }
        DBG("pass=%d npk=%d nwat=%d\n",ipass,npk,nwat);
        /* decode each */
        int iifac=8;
        for(j=0;j<nwat;j++){
            if(ab&&ab())return uniques?uniques:WSPR_ERR_ABORT;
            float f1=cand[j].freq,drift1=cand[j].drift; int shift1=cand[j].shift,not_decoded=1;
            int ib=1;
            while(ib<=nblocksize&&not_decoded){
                int blocksize,bitmetric;
                if(ib<4){blocksize=ib;bitmetric=0;} else {blocksize=1;bitmetric=1;}
                int idt=0,ii=0;
                while(not_decoded&&idt<=(128/iifac)){
                    if(ab&&ab())return uniques?uniques:WSPR_ERR_ABORT;   /* one jitter step = demod + Fano: poll every step */
                    ii=(idt+1)/2; if(idt%2==1)ii=-ii; ii*=iifac; int jittered=shift1+ii;
                    noncoh_detect(W,symbols,&f1,&jittered,&drift1,symfac,blocksize,bitmetric);
                    float sq=0; for(i=0;i<162;i++){float y=(float)symbols[i]-128.0f;sq+=y*y;} float rms=w_sqrtf(sq/162.0f);
                    if(rms>minrms){
                        deinterleave(symbols);
                        unsigned metric; not_decoded=fano(W->nodes,symbols,W->mettab,delta,maxcycles,decdata,&metric);
                        DBG("  wat=%d f=%.1f sh=%d dr=%.1f ib=%d idt=%d rms=%.1f fano=%d\n",j,f1,shift1,drift1,ib,idt,rms,!not_decoded);
                    } else DBG("  wat=%d rms=%.1f < minrms SKIP\n",j,rms);
                    idt++;
                }
                ib++;
            }
            if(!not_decoded){
                ndecodes_pass++;
                for(i=0;i<11;i++) message11[i]=(signed char)decdata[i];
                char clp[24],callsign[16]; int ndbm;
                int noprint=unpk_(message11,&W->ht,clp,callsign,&ndbm); (void)ndbm;
                DBG("  DECODED clp=[%s] call=[%s] noprint=%d\n",clp,callsign,noprint);
                /* subtraction */
                if(!noprint){
                    if(wspr_pack_for_subtract(clp,chan_sym)){
                        if(ab&&ab())return uniques?uniques:WSPR_ERR_ABORT;
                        subtract_signal2(W,f1,shift1,drift1,chan_sym,ab);
                    }
                }
                /* dedupe + output */
                int dupe=0; for(i=0;i<nuniq;i++){ float dfq=f1-allfreqs[i]; if(dfq<0)dfq=-dfq;
                    int same=1; { const char*a=callsign,*b=allcalls[i]; while(*a&&*b){if(*a!=*b){same=0;break;}a++;b++;} if(*a||*b)same=0; }
                    if(same&&dfq<4.0f)dupe=1; }
                if(!dupe&&!noprint&&uniques<maxout){
                    w_strcpy(allcalls[nuniq],callsign); allfreqs[nuniq]=f1; if(nuniq<99)nuniq++;
                    wspr_result_t*r=&out[uniques];
                    /* parse clp "call grid power" */
                    { int p=0,q=0; while(clp[p]&&clp[p]!=' ')r->call[q++]=clp[p++]; r->call[q]=0;
                      while(clp[p]==' ')p++;
                      q=0; r->grid[0]=0;
                      /* second token could be grid or (type2) power */
                      char t2[16]; int tl=0; while(clp[p]&&clp[p]!=' ')t2[tl++]=clp[p++]; t2[tl]=0;
                      while(clp[p]==' ')p++;
                      char t3[16]; int t3l=0; while(clp[p])t3[t3l++]=clp[p++]; t3[t3l]=0;
                      if(t3l>0){ w_strcpy(r->grid,t2); r->dbm=w_atoin(t3); }
                      else { r->grid[0]=0; r->dbm=w_atoin(t2); }
                    }
                    w_strcpy(r->msg,clp);
                    r->snr_db=cand[j].snr; r->dt_s=shift1*dt-1.0f; r->freq_hz=f1; r->drift_hz=drift1;
                    uniques++;
                }
            }
        }
#ifdef WSPR_DEBUG
        if(ipass==0&&getenv("WSPR_RESID")){ FILE*o=fopen(getenv("WSPR_RESID"),"wb"); for(i=0;i<W->npoints;i++){fwrite(&W->idat[i],2,1,o);fwrite(&W->qdat[i],2,1,o);} fclose(o); }
#endif
    }
    return uniques;
}

/* re-encode a decoded message to 162 tones for subtraction (uses wsprenc). */
extern int wspr_pack_message(const char *msg, uint8_t tones[162]);
int wspr_pack_for_subtract(const char *clp, uint8_t *chan){
    /* only re-encode messages wsprenc can represent (type 1/2); skip hashed */
    if(clp[0]=='<') return 0;
    return wspr_pack_message(clp,chan)==0;
}

/* ===================== work arena setup ============================= */
static void* bump(uint8_t*base,uint32_t*off,uint32_t wl,uint32_t bytes){
    uint32_t a=(bytes+7u)&~7u; if(*off+a>wl) return 0; void*p=base+*off; *off+=a; return p;
}
static uint32_t g_arena_peak=0;   /* bytes carved from the work buffer (reported) */
static int setup_arena(wctx_t*W,int npoints,void*work,uint32_t wl){
    uint8_t*base=(uint8_t*)work; uint32_t off=0;
    int nffts=4*(npoints/512)-1; if(nffts<1)nffts=1;
    int ntw=nffts; if(ntw>346)ntw=346;   /* band cap: 17*ntw+1024 <= scr floats */
    W->npoints=npoints; W->nffts=nffts; W->ntw=ntw;
    W->idat=(int16_t*)bump(base,&off,wl,WSPR_NPOINTS*sizeof(int16_t));
    W->qdat=(int16_t*)bump(base,&off,wl,WSPR_NPOINTS*sizeof(int16_t));
    W->scr =(float*)  bump(base,&off,wl,WSPR_SCR_FLOATS*sizeof(float));
    W->cand=(cand_t*) bump(base,&off,wl,200*sizeof(cand_t));
    W->nodes=(fnode_t*)bump(base,&off,wl,(FANO_NBITS+2)*sizeof(fnode_t));
    if(!W->idat||!W->qdat||!W->scr||!W->cand||!W->nodes) return WSPR_ERR_WORK;
    g_arena_peak=off;
    { int i; for(i=0;i<512;i++) W->wwin[i]=w_sinf(0.006147931*(double)i); }
    { int i; float bias=0.45f; for(i=0;i<256;i++){ W->mettab[0][i]=w_iround(10.0f*(wspr_mtab2[i]-bias)); W->mettab[1][i]=w_iround(10.0f*(wspr_mtab2[255-i]-bias)); } }
    fft_tw_init(&W->tw);
    return 0;
}
/* peak bytes used from the caller's work buffer (0 until a decode has run). */
uint32_t wspr_work_peak(void){ return g_arena_peak; }

/* one shared decode context (the decoder is single-threaded, one period at a time) */
static wctx_t W;

/* candidate search window of the I/Q entry points, offset from 1500 Hz (default: the whole +-150 Hz) */
static float g_fmin=-150.0f, g_fmax=150.0f;
void wspr_set_band(float f_lo,float f_hi){
    if(f_lo<-150.0f)f_lo=-150.0f;
    if(f_hi>150.0f)f_hi=150.0f;
    if(f_lo>=f_hi){f_lo=-150.0f;f_hi=150.0f;}
    g_fmin=f_lo; g_fmax=f_hi;
}

/* float 375-sps I/Q (e.g. a .c2 file): auto-scaled into the s16 work arena. */
int wspr_decode_iq(float *idat,float *qdat,int npoints,wspr_result_t*out,int maxout,
                   void*work,uint32_t wl,int(*ab)(void)){
    int rc,i; float mx=1e-9f; float scale;
    if(!idat||!qdat||!out||!work) return WSPR_ERR_ARG;
    if(npoints>(int)WSPR_NPOINTS)npoints=WSPR_NPOINTS;
    if(wl<WSPR_WORK_MIN) return WSPR_ERR_WORK;
    rc=setup_arena(&W,npoints,work,wl); if(rc)return rc;
    for(i=0;i<npoints;i++){ float a=idat[i]<0?-idat[i]:idat[i],b=qdat[i]<0?-qdat[i]:qdat[i];
        if(a>mx)mx=a;
        if(b>mx)mx=b; }
    scale=24000.0f/mx;   /* decode is scale-invariant; pick precision, avoid clip */
    for(i=0;i<npoints;i++){ W.idat[i]=clamp16(idat[i]*scale); W.qdat[i]=clamp16(qdat[i]*scale); }
    for(i=npoints;i<(int)WSPR_NPOINTS;i++){W.idat[i]=0;W.qdat[i]=0;}
    return decode_core(&W,g_fmin,g_fmax,out,maxout,ab);
}

/* s16 375-sps I/Q already produced by the loader (e.g. streamed down-convert
 * during capture): decoded in place from the arena.  Scale is irrelevant
 * (the decoder is scale-invariant); any consistent I/Q scale works. */
int wspr_decode_iq16(const int16_t *idat,const int16_t *qdat,int npoints,
                     wspr_result_t*out,int maxout,void*work,uint32_t wl,int(*ab)(void)){
    int rc,i;
    if(!idat||!qdat||!out||!work) return WSPR_ERR_ARG;
    if(npoints>(int)WSPR_NPOINTS)npoints=WSPR_NPOINTS;
    if(wl<WSPR_WORK_MIN) return WSPR_ERR_WORK;
    rc=setup_arena(&W,npoints,work,wl); if(rc)return rc;
    for(i=0;i<npoints;i++){W.idat[i]=idat[i];W.qdat[i]=qdat[i];}
    for(i=npoints;i<(int)WSPR_NPOINTS;i++){W.idat[i]=0;W.qdat[i]=0;}
    return decode_core(&W,g_fmin,g_fmax,out,maxout,ab);
}

/* s16 375-sps I/Q streamed straight into the arena by a fill callback, so the
 * caller keeps no second 180 KB I/Q buffer (see wsprdec.h).  Same decode_core
 * as wspr_decode_iq16; only the I/Q source differs. */
int wspr_decode_iq16_cb(int (*fill_iq)(void*,int16_t*,int16_t*,int),void*ctx,
                        int npoints,wspr_result_t*out,int maxout,
                        void*work,uint32_t wl,int(*ab)(void)){
    int rc,got,i;
    if(!fill_iq||!out||!work) return WSPR_ERR_ARG;
    if(npoints>(int)WSPR_NPOINTS)npoints=WSPR_NPOINTS;
    if(npoints<0)npoints=0;
    if(wl<WSPR_WORK_MIN) return WSPR_ERR_WORK;
    rc=setup_arena(&W,npoints,work,wl); if(rc)return rc;
    got=fill_iq(ctx,W.idat,W.qdat,npoints);
    if(got<0) return WSPR_ERR_IO;
    if(got>npoints)got=npoints;
    for(i=got;i<(int)WSPR_NPOINTS;i++){W.idat[i]=0;W.qdat[i]=0;}
    return decode_core(&W,g_fmin,g_fmax,out,maxout,ab);
}

/* ===================== 8 kHz down-converter ========================= */
/* mix at 1500 Hz, resample 8000->375 via a 3-phase polyphase windowed sinc,
 * store s16 into the arena.  tap[3][2*DS_R] and the input read cache live in
 * the work-arena scratch (W->scr), so this path adds no static BSS. */
#define DS_R 192            /* half filter length (taps per phase = 2*DS_R) */
/* the 1500 Hz mix (period 16 samples) and the 3-phase FIR, shared by the file path below and the streaming
 * down-converter (wspr_dc_*): both produce the same I/Q from the same audio */
static void dc_tables(float *tap,float *cmix,float *smix){
    int i,m;
    for(i=0;i<16;i++){ float s,c; w_sincos(W_TWO_PI*1500.0*(double)i/8000.0,&s,&c); cmix[i]=c; smix[i]=-s; }
    float fc=180.0f, Ts=2.0f*fc/8000.0f;
    for(m=0;m<3;m++){ float frac=(float)m/3.0f; int j;
        for(j=0;j<2*DS_R;j++){ float t=frac+(float)(DS_R-1-j);
            float x=Ts*t; float sinc=(x==0.0f)?1.0f:(w_sinf((double)(3.14159265358979f*x))/(3.14159265358979f*x));
            float wn=0.54f-0.46f*w_cosf(W_TWO_PI*(double)(j)/(double)(2*DS_R-1));
            tap[m*2*DS_R+j]=Ts*sinc*wn; }
    }
}

/* ---- streaming down-converter (see wsprdec.h) ---- */
void wspr_dc_init(wspr_dc_t*d){ dc_tables(d->tap,d->cmix,d->smix); wspr_dc_reset(d,1.0f,0); }
void wspr_dc_reset(wspr_dc_t*d,float gain,uint32_t maxpoints){
    int i; d->nin=0; d->mout=0; d->need=DS_R+1; d->phase=0; d->i0=0; d->clip=0;
    d->gain=gain>0.0f?gain:1.0f; d->cap=maxpoints>WSPR_NPOINTS?WSPR_NPOINTS:maxpoints;
    for(i=0;i<WSPR_DC_RING;i++)d->ring[i]=0.0f;
}
static void dc_emit(wspr_dc_t*d,wspr_dc_put_t put,void*ctx){
    while(d->mout<d->cap&&d->nin>=d->need){            /* output m needs input up to i0+DS_R */
        const float*tp=d->tap+d->phase*2*DS_R; float acc_i=0.0f,acc_q=0.0f; int j,i0=(int)d->i0;
        for(j=0;j<2*DS_R;j++){ int n=i0-(DS_R-1)+j;
            if(n<0)continue;
            float xv=d->ring[(uint32_t)n&(WSPR_DC_RING-1)], t=tp[j], xm=xv*t; int mi=n&15;
            acc_i+=xm*d->cmix[mi]; acc_q+=xm*d->smix[mi];
        }
        acc_i*=d->gain; acc_q*=d->gain;
        if(acc_i>32767.0f||acc_i<-32768.0f||acc_q>32767.0f||acc_q<-32768.0f)d->clip++;
        put(ctx,clamp16(acc_i),clamp16(acc_q));
        d->mout++;
        { uint32_t c64=d->mout*64u; d->phase=c64%3u; d->i0=(c64-d->phase)/3u; d->need=d->i0+DS_R+1; }
    }
}
void wspr_dc_push(wspr_dc_t*d,const int16_t*x,uint32_t n,wspr_dc_put_t put,void*ctx){
    uint32_t i;
    for(i=0;i<n;i++){
        d->ring[d->nin&(WSPR_DC_RING-1)]=x?(float)x[i]:0.0f; d->nin++;
        if(d->nin>=d->need)dc_emit(d,put,ctx);
    }
}

static int downconvert(wctx_t*W,const dsrc_t*src){
    float cmix[16],smix[16]; int i,m;
    float *tap=W->scr;          /* 3*2*DS_R = 1152 floats */
    float *cache=W->scr+3*2*DS_R; const int CACHEN=4096; /* 1152..5247 */
    dc_tables(tap,cmix,smix);
    uint32_t Nin=src->len; int Nout=(int)WSPR_NPOINTS;
    uint32_t cache_start=0xFFFFFFFFu, cache_len=0;
    /* scale from the whole-input peak (one sequential prescan read of the
     * capture).  Output of the unity-gain LPF is <= input, so mapping input
     * peak to ~16000 keeps the s16 I/Q well-resolved and clip-free.  NOTE: this
     * raw-8 kHz path thus reads the capture twice; the recommended IC-705 path
     * (wspr_decode_iq16, s16 I/Q from a down-convert streamed during capture)
     * does neither prescan nor decode-time re-read of the 8 kHz samples. */
    float scale=1.0f;
    { float mxin=1.0f; uint32_t s;
      for(s=0;s<Nin;s+=CACHEN){ uint32_t cl=CACHEN; if(s+cl>Nin)cl=Nin-s;
          int got=src->read(src->ctx,s,cache,cl); if(got<0)return WSPR_ERR_IO; if((uint32_t)got<cl)cl=got;
          for(i=0;i<(int)cl;i++){ float a=cache[i]<0?-cache[i]:cache[i]; if(a>mxin)mxin=a; } }
      scale=20000.0f/mxin; if(scale<0.25f)scale=0.25f; if(scale>256.0f)scale=256.0f;
      cache_start=0xFFFFFFFFu; cache_len=0; }
    for(m=0;m<Nout;m++){
        long long c64=(long long)m*64; int phase=(int)(c64%3); if(phase<0)phase+=3;
        int i0=(int)((c64-phase)/3); float acc_i=0.0f,acc_q=0.0f; int j;
        for(j=0;j<2*DS_R;j++){ int n=i0-(DS_R-1)+j;
            if(n<0||n>=(int)Nin)continue;
            if((uint32_t)n<cache_start||(uint32_t)n>=cache_start+cache_len){
                cache_start=(uint32_t)n; cache_len=CACHEN; if(cache_start+cache_len>Nin)cache_len=Nin-cache_start;
                int got=src->read(src->ctx,cache_start,cache,cache_len); if(got<0)return WSPR_ERR_IO; if((uint32_t)got<cache_len)cache_len=got;
            }
            float xv=cache[n-(int)cache_start], t=tap[phase*2*DS_R+j], xm=xv*t; int mi=n&15;
            acc_i+=xm*cmix[mi]; acc_q+=xm*smix[mi];
        }
        W->idat[m]=clamp16(acc_i*scale); W->qdat[m]=clamp16(acc_q*scale);
    }
    return 0;
}

int wspr_decode(const dsrc_t*src,float f_lo,float f_hi,float nominal_start_s,
                wspr_result_t*out,int maxout,void*work,uint32_t wl,int(*ab)(void)){
    int rc; (void)nominal_start_s;
    if(!src||!src->read||!out||!work) return WSPR_ERR_ARG;
    if(wl<WSPR_WORK_MIN) return WSPR_ERR_WORK;
    if(src->fs!=8000) return WSPR_ERR_ARG;
    rc=setup_arena(&W,WSPR_NPOINTS,work,wl); if(rc)return rc;
    rc=downconvert(&W,src); if(rc)return rc;
    return decode_core(&W,f_lo,f_hi,out,maxout,ab);
}
