/* Background-image / translucency ABI for the stock launcher (profile "b31",
 * /usr/bin/zte_topsw_devui sha256 e3914e78a8488cb736770f0ac9fb8ce10e0e5222fa50285f08e9e8be90d7f1e9).
 * Hand-written, kept separate from the generated abi.h on purpose.
 * Every function is verified word-for-word at runtime before use; on any
 * mismatch the background feature is disabled and the old solid look is used.
 *
 * Launcher is LVGL v8.3.x (TUGL C++ wrapper on top), LV_COLOR_DEPTH 16,
 * LV_COLOR_16_SWAP 0  => lv_img_dsc_t LV_IMG_CF_TRUE_COLOR = RGB565 little endian.
 */
#ifndef ZTE_LAUNCHER_ABI_BG_H
#define ZTE_LAUNCHER_ABI_BG_H
#include <stdint.h>
#include <stddef.h>

/* wrapper(this, const void *src): lv_obj_set_style_bg_img_src(this->obj, src, 0)  (prop 40) */
#define BG_SET_BG_IMG_SRC   0x5378a0
/* wrapper(this, int w): lv_obj_set_style_border_width(this->obj, w, 0)            (prop 50) */
#define BG_SET_BORDER_WIDTH 0x537a44
/* wrapper(this, r, g, b): lv_obj_set_style_border_color(this->obj, rgb565, 0)     (prop 48) */
#define BG_SET_BORDER_RGB   0x537ab4
/* raw LVGL: lv_obj_set_style_border_opa(lv_obj_t *, lv_opa_t, selector)          (prop 49) */
#define BG_LV_BORDER_OPA    0x4378f8

static const uint32_t bg_w_bg_img_src[] = {0xa9be7bfd, 0x910003fd, 0xf9000bf3, 0xaa0103f3, 0xb0004a21, 0xf941ec21, 0xd63f0020, 0xaa1303e1, 0x52800002, 0xf9400bf3, 0xa8c27bfd, 0x90004a30, 0xf944ce10, 0xd61f0200};
static const uint32_t bg_w_border_width[] = {0xa9be7bfd, 0x910003fd, 0xf9000bf3, 0x2a0103f3, 0xb0004a21, 0xf941ec21, 0xd63f0020, 0x2a1303e1, 0x52800002, 0xf9400bf3, 0xa8c27bfd, 0xb0004a30, 0xf942fa10, 0xd61f0200};
static const uint32_t bg_w_border_rgb[] = {0xa9be7bfd, 0x53021c42, 0x53031c63, 0x53031c21, 0x910003fd, 0x2a021462, 0xf9000bf3, 0x2a012c53, 0xb0004a21, 0xf941ec21, 0xd63f0020, 0x2a1303e1, 0x52800002, 0xf9400bf3, 0xa8c27bfd, 0xb0004a30, 0xf943d610, 0xd61f0200};
static const uint32_t bg_w_border_opa[] = {0xb0005210, 0xf9462a10, 0x2a0203e3, 0xd2800004, 0xd2800002, 0x12001c21, 0xb3607c82, 0xb3407c22, 0x52800621, 0xd61f0200};
/* the LVGL style stubs the wrappers tail-call into, and their prop ids */
static const uint32_t bg_w_lv_bg_img_src[] = {0xb0005210, 0xf9462a10, 0x2a0203e3, 0xaa0103e2, 0x52800501, 0xd61f0200};
static const uint32_t bg_w_lv_border_width[] = {0xb0005210, 0xf9462a10, 0x2a0203e3, 0xd2800004, 0xd2800002, 0x13003c21, 0xb3607c82, 0xb3407c22, 0x52800641, 0xd61f0200};
static const uint32_t bg_w_lv_border_color[] = {0xb0005210, 0xf9462a10, 0x2a0203e3, 0x92403c22, 0x52800601, 0xd61f0200};

static const struct { uintptr_t addr; const uint32_t *words; size_t n; } bg_code_checks[] = {
 {BG_SET_BG_IMG_SRC, bg_w_bg_img_src, sizeof bg_w_bg_img_src / 4},
 {BG_SET_BORDER_WIDTH, bg_w_border_width, sizeof bg_w_border_width / 4},
 {BG_SET_BORDER_RGB, bg_w_border_rgb, sizeof bg_w_border_rgb / 4},
 {BG_LV_BORDER_OPA, bg_w_border_opa, sizeof bg_w_border_opa / 4},
 {0x437838, bg_w_lv_bg_img_src, sizeof bg_w_lv_bg_img_src / 4},
 {0x437920, bg_w_lv_border_width, sizeof bg_w_lv_border_width / 4},
 {0x4378e0, bg_w_lv_border_color, sizeof bg_w_lv_border_color / 4},
};
/* GOT slots used by the code above (non-PIE binary, fixed). */
static const struct { uintptr_t slot, target; } bg_slot_checks[] = {
 {0xe7b998, 0x437838}, /* wrapper -> lv_obj_set_style_bg_img_src */
 {0xe7c5f0, 0x437920}, /* wrapper -> lv_obj_set_style_border_width */
 {0xe7c7a8, 0x4378e0}, /* wrapper -> lv_obj_set_style_border_color */
 {0xe78c50, 0x436ab0}, /* stubs -> lv_obj_set_local_style_prop */
 {0xe7c3d8, 0x539c78}, /* wrapper -> get lv_obj_t* (this+8) */
 {0xe7e360, 0x437740}, /* existing bg-color wrapper 0x53793c -> lv_obj_set_style_bg_opa */
};
static inline int bg_abi_valid(uintptr_t (*read_pointer)(uintptr_t), uint32_t (*read_word)(uintptr_t)) {
 for (size_t i = 0; i < sizeof bg_code_checks / sizeof bg_code_checks[0]; i++)
  for (size_t k = 0; k < bg_code_checks[i].n; k++)
   if (read_word(bg_code_checks[i].addr + 4 * k) != bg_code_checks[i].words[k]) return 0;
 for (size_t i = 0; i < sizeof bg_slot_checks / sizeof bg_slot_checks[0]; i++)
  if (read_pointer(bg_slot_checks[i].slot) != bg_slot_checks[i].target) return 0;
 return 1;
}

/* LVGL v8 lv_img_dsc_t (header bitfield: cf:5 always_zero:3 reserved:2 w:11 h:11) */
typedef struct {
 uint32_t header;
 uint32_t data_size;
 const uint8_t *data;
} bg_img_dsc_t;
#define BG_CF_TRUE_COLOR 4u
#define BG_IMG_HEADER(w, h) (BG_CF_TRUE_COLOR | ((uint32_t)(w) << 10) | ((uint32_t)(h) << 21))
#endif
