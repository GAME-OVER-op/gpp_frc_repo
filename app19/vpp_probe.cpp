// Phase 19a - native VPP FRC probe (RedMagic 9 Pro / SM8650, libvpplibrary.so)
// Cel: bezopasno (BEZ ocheredi buferov) proverit:
//  1) zagruzhaetsya li rodnaya libvpplibrary.so v process i nahodyatsya li vpp_* simvoly;
//  2) init -> set_parameter(IN/OUT NV12_Venus 1280x720);
//  3) perebrat hqv_control (AUTO + MANUAL po ctrl_type 0..11) i zalogirovat,
//     kakoj ctrl_type prinimaetsya kak FRC (rc==0);
//  4) snyat tochnye trebovaniya buferov (vpp_requirements) - raw dump u32,
//     chtoby uvidet Required-razmer s extradata (ozhidaem ~1445888 / +32768).
// Callbacks ne vyzyvayutsya (net queue_buf) => ABI callback'ov ne kritichno.
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <dlfcn.h>
#include <android/log.h>

#define TAG "VPP_PROBE"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

// ---- ABI (podtverzhdeno dizassemblerom libvpplibrary.so) ----
struct vpp_port_param { uint32_t width, height, stride, scanlines, fmt; }; // 20 b
struct vpp_buffer; struct vpp_event;
struct vpp_callbacks {            // 32 b (4 ukazatelya), peredaetsya po ssylke
	void *pv;
	void (*input_buffer_done)(void *, struct vpp_buffer *);
	void (*output_buffer_done)(void *, struct vpp_buffer *);
	void (*vpp_event)(void *, void *);
};
struct hqv_ctrl_frc { uint32_t mode, level, interp; };
struct hqv_control {             // mode@0, ctrl_type@4, union@8 (do 272 b)
	uint32_t mode;
	uint32_t ctrl_type;
	union { struct hqv_ctrl_frc frc; uint8_t _pad[264]; } u;
};

typedef void *  (*fn_init)(uint32_t, struct vpp_callbacks);
typedef uint32_t(*fn_setp)(void *, uint32_t, struct vpp_port_param);
typedef uint32_t(*fn_setc)(void *, struct hqv_control, void *);
typedef uint32_t(*fn_getr)(void *, void *);
typedef uint32_t(*fn_u1)(void *);
typedef uint32_t(*fn_v)(void);

static void stub_ibd(void *, struct vpp_buffer *) {}
static void stub_obd(void *, struct vpp_buffer *) {}
static void stub_evt(void *, void *) {}

static void dumpu32(const char *w, const uint8_t *p, int n) {
	const uint32_t *u = (const uint32_t *)p;
	char l[256];
	for (int i = 0; i < n / 4; i += 6) {
		int o = snprintf(l, sizeof l, "%s u32[%02d]:", w, i);
		for (int j = 0; j < 6 && i + j < n / 4; j++)
			o += snprintf(l + o, sizeof(l) - o, " %u", u[i + j]);
		LOGI("%s", l);
	}
}

int main() {
	LOGI("=== VPP probe (Phase 19a) start ===");
	void *h = dlopen("libvpplibrary.so", RTLD_NOW | RTLD_GLOBAL);
	if (!h) { LOGI("dlopen FAIL: %s", dlerror()); return 1; }
	fn_init p_init = (fn_init)dlsym(h, "vpp_init");
	fn_setp p_setp = (fn_setp)dlsym(h, "vpp_set_parameter");
	fn_setc p_setc = (fn_setc)dlsym(h, "vpp_set_ctrl");
	fn_getr p_getr = (fn_getr)dlsym(h, "vpp_get_buf_requirements");
	fn_u1   p_close = (fn_u1)dlsym(h, "vpp_close");
	fn_u1   p_shut  = (fn_u1)dlsym(h, "vpp_shutdown");
	fn_v    p_boot  = (fn_v)dlsym(h, "vpp_boot");
	LOGI("sym init=%p setp=%p setc=%p getr=%p close=%p shut=%p boot=%p",
	     p_init, p_setp, p_setc, p_getr, p_close, p_shut, p_boot);
	if (!p_init || !p_setc) { LOGI("missing core symbol"); return 2; }

	struct vpp_callbacks cbs; memset(&cbs, 0, sizeof cbs);
	cbs.pv = (void *)0x1;
	cbs.input_buffer_done = stub_ibd;
	cbs.output_buffer_done = stub_obd;
	cbs.vpp_event = stub_evt;

	LOGI("vpp_init(flags=0, cbs@%p) ...", &cbs);
	void *ctx = p_init(0u, cbs);
	LOGI("vpp_init -> ctx=%p", ctx);
	if (!ctx && p_boot) {
		uint32_t b = p_boot(); LOGI("vpp_boot=%u, retry init", b);
		ctx = p_init(0u, cbs); LOGI("vpp_init(2) -> ctx=%p", ctx);
	}
	if (!ctx) { LOGI("init NULL, abort"); return 3; }

	struct vpp_port_param in  = {1280, 720, 1280, 736, 0 /*NV12_VENUS*/};
	struct vpp_port_param out = {1280, 720, 1280, 736, 0};
	if (p_setp) {
		uint32_t r1 = p_setp(ctx, 0u, in);
		uint32_t r2 = p_setp(ctx, 1u, out);
		LOGI("set_parameter IN rc=%u  OUT rc=%u", r1, r2);
	}

	for (int autom = 1; autom >= 0; --autom) {
		int hi = autom ? 0 : 11;
		for (int ct = 0; ct <= hi; ++ct) {
			struct hqv_control c; memset(&c, 0, sizeof c);
			c.mode = autom ? 1u /*AUTO*/ : 2u /*MANUAL*/;
			c.ctrl_type = (uint32_t)ct;
			c.u.frc.mode = 1; c.u.frc.level = 3; c.u.frc.interp = 1;
			uint8_t reqs[1024]; memset(reqs, 0, sizeof reqs);
			uint32_t rc = p_setc(ctx, c, reqs);
			LOGI("set_ctrl(mode=%u ctrl_type=%d) rc=%u", c.mode, ct, rc);
			if (rc == 0) {
				dumpu32("  setc.reqs", reqs, 160);
				if (p_getr) {
					uint8_t rq[1024]; memset(rq, 0, sizeof rq);
					uint32_t g = p_getr(ctx, rq);
					LOGI("  get_buf_requirements rc=%u", g);
					dumpu32("  gbr", rq, 160);
				}
			}
		}
	}
	if (p_close) LOGI("vpp_close=%u", p_close(ctx));
	if (p_shut)  LOGI("vpp_shutdown=%u", p_shut(ctx));
	LOGI("=== VPP probe done ===");
	return 0;
}
