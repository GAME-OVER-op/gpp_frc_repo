// Phase 19b - native VPP use-case + FRC discovery (SM8650 libvpplibrary.so)
// Naydeno v 19a: VPP gruzitsya v process, vpp_init OK, no pstVppUsecase_Find
// vozvrashaet NULL dlya vyhoda pri flags=0 (realtime) -> set_parameter OUT rc=4,
// poetomu MANUAL set_ctrl ne rabotaet (pstUc==null). FRC = ctrl_type 6.
//
// Strategiya 19b:
//  1) SVIP flags vpp_init (0,1,2,3,4,8,0x10,0x20,0x40,0x100) - ishchem tot,
//     pri kotorom set_parameter OUT vozvrashaet 0 (use-case nayden; ozhidaem
//     non-realtime bit, sm. "FRC Factor ... Non-realtime").
//  2) Pri nevezenii - dlya kazhdogo flaga svip formata vyhoda (0..6) i
//     varianta vyhod=2x po fps cherez vpp_set_vid_prop (esli est).
//  3) Tolko kogda use-case nayden -> set_ctrl AUTO, potom MANUAL FRC (ct=6),
//     potom get_buf_requirements -> raw u32 dump (zhdem Required ~1445888, +32768).
//  4) Vse vyzovy set_ctrl/set_parameter zashchisheny siglongjmp - probe ne padaet
//     celikom dazhe pri SIGSEGV vnutri biblioteki.
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <dlfcn.h>
#include <setjmp.h>
#include <signal.h>
#include <android/log.h>

#define TAG "VPP_PROBE"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

struct vpp_port_param { uint32_t width, height, stride, scanlines, fmt; };
struct vpp_buffer;
struct vpp_callbacks { void *pv; void (*ibd)(void*,vpp_buffer*); void (*obd)(void*,vpp_buffer*); void (*evt)(void*,void*); };

typedef void *  (*fn_init)(uint32_t, struct vpp_callbacks);
typedef uint32_t(*fn_setp)(void *, uint32_t, struct vpp_port_param);
typedef uint32_t(*fn_setc)(void *, void * /*hqv_control by-ref via x1*/, void *);
typedef uint32_t(*fn_getr)(void *, void *);
typedef uint32_t(*fn_u1)(void *);
typedef uint32_t(*fn_v)(void);
typedef uint32_t(*fn_vidprop)(void *, void *);

static fn_init p_init; static fn_setp p_setp; static fn_setc p_setc;
static fn_getr p_getr; static fn_u1 p_close, p_open; static fn_u1 p_shut; static fn_v p_boot;

static void stub_ibd(void*,vpp_buffer*){}
static void stub_obd(void*,vpp_buffer*){}
static void stub_evt(void*,void*){}

static sigjmp_buf g_jb; static volatile int g_inprot;
static void on_sig(int s){ if(g_inprot) siglongjmp(g_jb,s); _exit(140); }

static void dumpu32(const char*w,const uint8_t*p,int n){
	const uint32_t*u=(const uint32_t*)p; char l[256];
	for(int i=0;i<n/4;i+=6){int o=snprintf(l,sizeof l,"%s u32[%02d]:",w,i);
		for(int j=0;j<6&&i+j<n/4;j++)o+=snprintf(l+o,sizeof(l)-o," %u",u[i+j]);LOGI("%s",l);} }

// hqv_control: mode@0, ctrl_type@4, ostalnoe zanulyaem (320 b s zapasom).
static void mk_ctrl(uint8_t*c,uint32_t mode,uint32_t ct){ memset(c,0,320); *(uint32_t*)(c)=mode; *(uint32_t*)(c+4)=ct; }

#define PROT(stmt, crashmsg) do{ g_inprot=1; int _s=sigsetjmp(g_jb,1); \
	if(_s==0){ stmt; } else { LOGI("  >>> CRASH(sig=%d) %s", _s, crashmsg); } g_inprot=0; }while(0)

static void *boot_init(uint32_t flags, vpp_callbacks &cbs){
	void*ctx=0; PROT(ctx=p_init(flags,cbs),"vpp_init");
	if(!ctx && p_boot){ uint32_t b=0; PROT(b=p_boot(),"vpp_boot"); LOGI("  vpp_boot=%u",b); PROT(ctx=p_init(flags,cbs),"vpp_init2"); }
	return ctx;
}

// vozvrashaet rc OUT (0 = use-case nayden)
static int set_ports(void*ctx,uint32_t ofmt){
	struct vpp_port_param in={1280,720,1280,736,0};
	struct vpp_port_param out={1280,720,1280,736,ofmt};
	uint32_t r1=99,r2=99;
	PROT(r1=p_setp(ctx,0u,in),"setp IN");
	PROT(r2=p_setp(ctx,1u,out),"setp OUT");
	LOGI("  set_parameter IN rc=%u  OUT(fmt=%u) rc=%u",r1,ofmt,r2);
	return (int)r2;
}

static void explore_frc(void*ctx){
	LOGI("  >>> USE-CASE OK: probing FRC controls");
	uint8_t ctrl[320]; uint8_t reqs[1024];
	// AUTO
	mk_ctrl(ctrl,1u,0u); memset(reqs,0,sizeof reqs); uint32_t rc=99;
	PROT(rc=p_setc(ctx,ctrl,reqs),"setc AUTO"); LOGI("  set_ctrl AUTO rc=%u",rc);
	// MANUAL svip ct=1..11 (6 ozhidaem = FRC)
	for(int ct=1;ct<=11;ct++){
		mk_ctrl(ctrl,2u,(uint32_t)ct); memset(reqs,0,sizeof reqs); rc=99;
		PROT(rc=p_setc(ctx,ctrl,reqs),"setc MANUAL");
		LOGI("  set_ctrl(MANUAL ct=%d) rc=%u",ct,rc);
		if(rc==0){ dumpu32("    reqs",reqs,160);
			if(p_getr){ uint8_t rq[1024]; memset(rq,0,sizeof rq); uint32_t g=99;
				PROT(g=p_getr(ctx,rq),"getr"); LOGI("    get_buf_requirements rc=%u",g); dumpu32("    gbr",rq,200);} }
	}
}

int main(){
	LOGI("=== VPP probe (Phase 19b: usecase+flags sweep) ===");
	struct sigaction sa; memset(&sa,0,sizeof sa); sa.sa_handler=on_sig; sigemptyset(&sa.sa_mask);
	sigaction(SIGSEGV,&sa,0); sigaction(SIGBUS,&sa,0); sigaction(SIGILL,&sa,0); sigaction(SIGABRT,&sa,0);

	void*h=dlopen("libvpplibrary.so",RTLD_NOW|RTLD_GLOBAL);
	if(!h){LOGI("dlopen FAIL: %s",dlerror());return 1;}
	p_init=(fn_init)dlsym(h,"vpp_init"); p_setp=(fn_setp)dlsym(h,"vpp_set_parameter");
	p_setc=(fn_setc)dlsym(h,"vpp_set_ctrl"); p_getr=(fn_getr)dlsym(h,"vpp_get_buf_requirements");
	p_close=(fn_u1)dlsym(h,"vpp_close"); p_open=(fn_u1)dlsym(h,"vpp_open");
	p_shut=(fn_u1)dlsym(h,"vpp_shutdown"); p_boot=(fn_v)dlsym(h,"vpp_boot");
	LOGI("sym init=%p setp=%p setc=%p getr=%p open=%p close=%p shut=%p boot=%p",
	     p_init,p_setp,p_setc,p_getr,p_open,p_close,p_shut,p_boot);
	if(!p_init||!p_setp||!p_setc){LOGI("missing core sym");return 2;}

	struct vpp_callbacks cbs; memset(&cbs,0,sizeof cbs);
	cbs.pv=(void*)0x1; cbs.ibd=stub_ibd; cbs.obd=stub_obd; cbs.evt=stub_evt;

	const uint32_t flags[]={0,1,2,3,4,8,0x10,0x20,0x40,0x100};
	int found=0;
	for(unsigned fi=0; fi<sizeof(flags)/sizeof(flags[0]); ++fi){
		uint32_t fl=flags[fi];
		LOGI("---- vpp_init(flags=0x%x) ----",fl);
		void*ctx=boot_init(fl,cbs);
		LOGI("  ctx=%p",ctx);
		if(!ctx) continue;
		if(p_open){ uint32_t o=99; PROT(o=p_open(ctx),"vpp_open"); LOGI("  vpp_open rc=%u",o); }
		int out=set_ports(ctx,0u); // NV12_VENUS out
		if(out==0){ found=1; explore_frc(ctx); }
		if(p_close) PROT(p_close(ctx),"close");
		if(p_shut)  PROT(p_shut(ctx),"shutdown");
	}

	if(!found){
		LOGI("==== pass2: flag x output-format sweep ====");
		const uint32_t f2[]={1,3,0x10};       // veroyatnye non-realtime/measure flagi
		const uint32_t ofmts[]={0,1,2,3,4,5,6};// NV12V,NV21V,P010,UBWC_NV12,UBWC_4R,RGBA,UBWC_RGBA
		for(unsigned a=0;a<sizeof(f2)/sizeof(f2[0]);++a)for(unsigned b=0;b<sizeof(ofmts)/sizeof(ofmts[0]);++b){
			uint32_t fl=f2[a];
			void*ctx=boot_init(fl,cbs); if(!ctx)continue;
			if(p_open){uint32_t o=99;PROT(o=p_open(ctx),"open");}
			LOGI("-- flags=0x%x ofmt=%u --",fl,ofmts[b]);
			int out=set_ports(ctx,ofmts[b]);
			if(out==0){found=1; explore_frc(ctx);}
			if(p_close)PROT(p_close(ctx),"close"); if(p_shut)PROT(p_shut(ctx),"shutdown");
			if(found)break;
		}
	}
	LOGI("=== VPP probe 19b done (found_usecase=%d) ===",found);
	return 0;
}
