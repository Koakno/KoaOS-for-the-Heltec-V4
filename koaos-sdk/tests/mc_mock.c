/* Host harness for apps/meshcore: mocks the KoaOS calls the app uses and plays a script of
 * radio packets / typed lines into it. The script is built by tests/mc_app_test.py, which also
 * checks the packets the app transmits (decrypting them independently with python-cryptography).
 *
 *   script lines:  RX <hex>      a packet arrives over the air
 *                  KEY <text>    the user types a line
 *   env: MC_SCRIPT=<file>  MC_DATA=<dir for the app's files>  MC_NOW=<unix time>
 * Output: the app's console on stdout, transmitted packets as "TX <hex>" lines on stdout. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "koaos.h"
#include "../apps/meshcore/mc_crypto.h"

/* ---------- minimal SHA-256 / HMAC / AES-128 (independent of the app's code) ---------- */
static const uint32_t K256[64] = {
0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
#define R32(x,n) (((x)>>(n))|((x)<<(32-(n))))
typedef struct { uint32_t h[8]; uint8_t b[64]; size_t n; uint64_t tot; } sha256_t;
static void s256_blk(uint32_t *h, const uint8_t *p) {
  uint32_t w[64], a,b,c,d,e,f,g,hh,t1,t2; int i;
  for (i=0;i<16;i++) w[i]=(uint32_t)p[4*i]<<24|(uint32_t)p[4*i+1]<<16|(uint32_t)p[4*i+2]<<8|p[4*i+3];
  for (i=16;i<64;i++){uint32_t s0=R32(w[i-15],7)^R32(w[i-15],18)^(w[i-15]>>3),s1=R32(w[i-2],17)^R32(w[i-2],19)^(w[i-2]>>10);w[i]=w[i-16]+s0+w[i-7]+s1;}
  a=h[0];b=h[1];c=h[2];d=h[3];e=h[4];f=h[5];g=h[6];hh=h[7];
  for (i=0;i<64;i++){t1=hh+(R32(e,6)^R32(e,11)^R32(e,25))+((e&f)^(~e&g))+K256[i]+w[i];t2=(R32(a,2)^R32(a,13)^R32(a,22))+((a&b)^(a&c)^(b&c));hh=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;}
  h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=hh;
}
static void s256_init(sha256_t *s){static const uint32_t iv[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};memcpy(s->h,iv,32);s->n=0;s->tot=0;}
static void s256_upd(sha256_t *s,const void *d,size_t n){const uint8_t*p=d;s->tot+=n;while(n){size_t t=64-s->n;if(t>n)t=n;memcpy(s->b+s->n,p,t);s->n+=t;p+=t;n-=t;if(s->n==64){s256_blk(s->h,s->b);s->n=0;}}}
static void s256_fin(sha256_t *s,uint8_t o[32]){uint64_t bits=s->tot*8;s->b[s->n++]=0x80;if(s->n>56){while(s->n<64)s->b[s->n++]=0;s256_blk(s->h,s->b);s->n=0;}while(s->n<56)s->b[s->n++]=0;for(int i=0;i<8;i++)s->b[56+i]=(uint8_t)(bits>>(56-8*i));s256_blk(s->h,s->b);for(int i=0;i<32;i++)o[i]=(uint8_t)(s->h[i/4]>>(24-8*(i%4)));}
static void hmac256(const uint8_t*k,size_t kl,const uint8_t*m,size_t n,uint8_t o[32]){uint8_t kb[64]={0},ip[64],op[64],in[32];sha256_t s;if(kl>64){s256_init(&s);s256_upd(&s,k,kl);s256_fin(&s,kb);}else memcpy(kb,k,kl);for(int i=0;i<64;i++){ip[i]=kb[i]^0x36;op[i]=kb[i]^0x5c;}s256_init(&s);s256_upd(&s,ip,64);s256_upd(&s,m,n);s256_fin(&s,in);s256_init(&s);s256_upd(&s,op,64);s256_upd(&s,in,32);s256_fin(&s,o);}
static uint8_t sbox[256], inv_sbox[256];
static uint8_t xt(uint8_t x){return (uint8_t)((x<<1)^((x>>7)*0x1b));}
static uint8_t gmul(uint8_t a,uint8_t b){uint8_t p=0;while(b){if(b&1)p^=a;a=xt(a);b>>=1;}return p;}
static void aes_tables(void){uint8_t p=1,q=1;do{p=p^(uint8_t)(p<<1)^((p&0x80)?0x1b:0);q^=q<<1;q^=q<<2;q^=q<<4;if(q&0x80)q^=0x09;uint8_t x=q^(uint8_t)((q<<1)|(q>>7))^(uint8_t)((q<<2)|(q>>6))^(uint8_t)((q<<3)|(q>>5))^(uint8_t)((q<<4)|(q>>4));sbox[p]=x^0x63;}while(p!=1);sbox[0]=0x63;for(int i=0;i<256;i++)inv_sbox[sbox[i]]=(uint8_t)i;}
static void aes_keyexp(const uint8_t*key,uint8_t rk[176]){memcpy(rk,key,16);uint8_t rc=1;for(int i=16;i<176;i+=4){uint8_t t[4]={rk[i-4],rk[i-3],rk[i-2],rk[i-1]};if(i%16==0){uint8_t u=t[0];t[0]=sbox[t[1]]^rc;t[1]=sbox[t[2]];t[2]=sbox[t[3]];t[3]=sbox[u];rc=xt(rc);}for(int j=0;j<4;j++)rk[i+j]=rk[i-16+j]^t[j];}}
static void aes_blk(const uint8_t rk[176],uint8_t s[16],int enc){
  int r;
  if(enc){for(int i=0;i<16;i++)s[i]^=rk[i];for(r=1;r<=10;r++){uint8_t t[16];for(int i=0;i<16;i++)t[i]=sbox[s[i]];
      uint8_t u[16];for(int c=0;c<4;c++)for(int rr=0;rr<4;rr++)u[4*c+rr]=t[4*((c+rr)%4)+rr];
      if(r<10)for(int c=0;c<4;c++){uint8_t*a=u+4*c,a0=a[0],a1=a[1],a2=a[2],a3=a[3];a[0]=xt(a0)^(xt(a1)^a1)^a2^a3;a[1]=a0^xt(a1)^(xt(a2)^a2)^a3;a[2]=a0^a1^xt(a2)^(xt(a3)^a3);a[3]=(xt(a0)^a0)^a1^a2^xt(a3);}
      for(int i=0;i<16;i++)s[i]=u[i]^rk[16*r+i];}}
  else{for(int i=0;i<16;i++)s[i]^=rk[160+i];for(r=9;r>=0;r--){uint8_t u[16];for(int c=0;c<4;c++)for(int rr=0;rr<4;rr++)u[4*((c+rr)%4)+rr]=s[4*c+rr];for(int i=0;i<16;i++)u[i]=inv_sbox[u[i]];for(int i=0;i<16;i++)u[i]^=rk[16*r+i];
      if(r>0)for(int c=0;c<4;c++){uint8_t*a=u+4*c,a0=a[0],a1=a[1],a2=a[2],a3=a[3];a[0]=gmul(a0,14)^gmul(a1,11)^gmul(a2,13)^gmul(a3,9);a[1]=gmul(a0,9)^gmul(a1,14)^gmul(a2,11)^gmul(a3,13);a[2]=gmul(a0,13)^gmul(a1,9)^gmul(a2,14)^gmul(a3,11);a[3]=gmul(a0,11)^gmul(a1,13)^gmul(a2,9)^gmul(a3,14);}
      memcpy(s,u,16);}}
}

/* ---------- script / mock state ---------- */
#define MAXEV 200
static struct { int rx; char *text; } ev[MAXEV]; static int nev, pos, keypos;
static const char *datadir; static int64_t fake_now; static uint32_t fake_ms;
static int hexv(int c){return c<='9'?c-'0':(c|32)-'a'+10;}

int32_t koa_abi_version(void){return 1;}
void koa_exit(int32_t rc){(void)rc;}
int32_t koa_yield(void){return pos >= nev ? KE_KILLED : KE_OK;}
void koa_sleep(int32_t ms){fake_ms += (uint32_t)ms;}
uint32_t koa_millis(void){return fake_ms;}
uint32_t koa_micros(void){return fake_ms*1000;}
int64_t koa_unixtime(void){return fake_now + fake_ms/1000;}
int32_t koa_random(void *b,int32_t n){for(int i=0;i<n;i++)((uint8_t*)b)[i]=(uint8_t)(rand()>>3);return 0;}
int32_t koa_acquire(uint32_t c){(void)c;return 0;} int32_t koa_release(uint32_t c){(void)c;return 0;}
int32_t koa_sha256(const void*in,int32_t n,uint8_t out[32]){sha256_t s;s256_init(&s);s256_upd(&s,in,(size_t)n);s256_fin(&s,out);return 0;}
int32_t koa_hmac_sha256(const void*k,int32_t kl,const void*in,int32_t n,uint8_t out[32]){hmac256(k,(size_t)kl,in,(size_t)n,out);return 0;}
int32_t koa_aes128_ecb(const uint8_t key[16],const void*in,void*out,int32_t n,int32_t enc){uint8_t rk[176];aes_keyexp(key,rk);for(int i=0;i<n;i+=16){uint8_t b[16];memcpy(b,(const uint8_t*)in+i,16);aes_blk(rk,b,enc);memcpy((uint8_t*)out+i,b,16);}return 0;}

/* files */
static FILE *fds[16];
int32_t koa_open(const char*p,int32_t fl){char path[512];snprintf(path,sizeof path,"%s/%s",datadir,p);FILE*f=fopen(path,fl==KO_RD?"rb":fl==KO_APP?"ab":"wb");if(!f)return KE_NOENT;for(int i=3;i<16;i++)if(!fds[i]){fds[i]=f;return i;}fclose(f);return KE_NOMEM;}
int32_t koa_close(int32_t fd){if(fd>=3&&fd<16&&fds[fd]){fclose(fds[fd]);fds[fd]=0;}return 0;}
int32_t koa_unlink(const char*p){(void)p;return 0;}
int32_t koa_readdir(const char*d,int32_t i,char*n,int32_t nm,int32_t*s){(void)d;(void)i;(void)n;(void)nm;(void)s;return KE_NOENT;}
int32_t koa_write(int32_t fd,const void*b,int32_t n){if(fd==1){fwrite(b,1,(size_t)n,stdout);return n;}if(fd>=3&&fd<16&&fds[fd])return (int32_t)fwrite(b,1,(size_t)n,fds[fd]);return KE_INVAL;}
int32_t koa_read(int32_t fd,void*b,int32_t n){if(fd>=3&&fd<16&&fds[fd])return (int32_t)fread(b,1,(size_t)n,fds[fd]);return KE_INVAL;}

/* keyboard: delivers the next KEY event as characters + newline */
int32_t koa_getc(int32_t ms){(void)ms;
  if (pos < nev && !ev[pos].rx) { char c = ev[pos].text[keypos]; if (c) { keypos++; return (unsigned char)c; } keypos = 0; pos++; return '\n'; }
  return -1; }

/* radio */
int32_t koa_lora_config(const koa_lora_cfg_t*c){printf("RADIO %.3f %.1f %d %d 0x%02x %d %d\n",c->freq_mhz,c->bw_khz,c->sf,c->cr,c->sync_word,c->power_dbm,c->preamble);return 0;}
int32_t koa_lora_cad(void){return 0;}
int32_t koa_lora_tx(const void*b,int32_t n){printf("TX ");for(int i=0;i<n;i++)printf("%02x",((const uint8_t*)b)[i]);printf("\n");return n;}
int32_t koa_lora_rx(void*b,int32_t nmax,int32_t to,float*rssi,float*snr){(void)to;
  if (pos < nev && ev[pos].rx) { const char*h=ev[pos].text; int n=(int)strlen(h)/2; if(n>nmax)n=nmax; for(int i=0;i<n;i++)((uint8_t*)b)[i]=(uint8_t)(hexv(h[2*i])*16+hexv(h[2*i+1])); pos++; *rssi=-92; *snr=6.5f; return n; }
  return 0; }

/* unused in this app but declared in the ABI */
int32_t koa_read_dummy(void){return 0;}
int32_t koa_ipc_listen(const char*n){(void)n;return KE_NOSYS;}
int32_t koa_ipc_recv(void*b,int32_t m,int32_t t,int32_t*c){(void)b;(void)m;(void)t;(void)c;return KE_NOSYS;}
int32_t koa_ipc_reply(int32_t c,const void*b,int32_t n){(void)c;(void)b;(void)n;return KE_NOSYS;}
int32_t koa_ipc_call(const char*n,const void*q,int32_t ql,void*r,int32_t rm,int32_t t){(void)n;(void)q;(void)ql;(void)r;(void)rm;(void)t;return KE_NOSYS;}
int32_t koa_gps_start(void){return KE_NOSYS;} int32_t koa_gps_stop(void){return KE_NOSYS;} int32_t koa_gps_info(koa_gps_t*o){(void)o;return KE_NOSYS;}
int32_t koa_wifi_status(char*a,int32_t b,char*c,int32_t d,int32_t*e){(void)a;(void)b;(void)c;(void)d;(void)e;return 0;}
int32_t koa_http_get(const char*u,void*b,int32_t n){(void)u;(void)b;(void)n;return KE_NOSYS;}

int main(void) {
  aes_tables();
  const char *sp = getenv("MC_SCRIPT"); datadir = getenv("MC_DATA"); fake_now = atoll(getenv("MC_NOW") ? getenv("MC_NOW") : "1760000000");
  FILE *f = fopen(sp, "r"); char line[1024];
  while (f && fgets(line, sizeof line, f) && nev < MAXEV) {
    line[strcspn(line, "\r\n")] = 0;
    if (!strncmp(line, "RX ", 3)) { ev[nev].rx = 1; ev[nev++].text = strdup(line + 3); }
    else if (!strncmp(line, "KEY ", 4)) { ev[nev].rx = 0; ev[nev++].text = strdup(line + 4); }
  }
  if (f) fclose(f);
  int rc = koa_main();
  printf("EXIT %d\n", rc);
  return 0;
}
