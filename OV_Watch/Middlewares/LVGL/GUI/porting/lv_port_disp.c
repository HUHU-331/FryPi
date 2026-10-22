/**
 * @file lv_port_disp_templ.c
 *
 */

 /*Copy this file as "lv_port_disp.c" and set this value to "1" to enable content*/
#if 1

/*********************
 *      INCLUDES
 *********************/
#include "lv_port_disp.h"
#include "lvgl.h"

#include "lcd.h"
#include "lcd_init.h"
#include "spi.h"

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/
static void disp_init(void);

static void disp_flush(lv_disp_drv_t * disp_drv, const lv_area_t * area, lv_color_t * color_p);
//static void gpu_fill(lv_disp_drv_t * disp_drv, lv_color_t * dest_buf, lv_coord_t dest_width,
//        const lv_area_t * fill_area, lv_color_t color);

/**********************
 *  STATIC VARIABLES
 **********************/
/* 正在等 DMA 完成回调的那次 flush；回调里用它来 ack LVGL */
static lv_disp_drv_t * g_current_disp_drv = NULL;

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

void lv_port_disp_init(void)
{
    /*-------------------------
     * Initialize your display
     * -----------------------*/
    // 这里进行你的屏幕初始化
    disp_init();

    /*-----------------------------
     * Create a buffer for drawing
     *----------------------------*/

    /**
     * LVGL requires a buffer where it internally draws the widgets.
     * Later this buffer will passed to your display driver's `flush_cb` to copy its content to your display.
     * The buffer has to be greater than 1 display row
     *
     * There are 3 buffering configurations:
     * 1. Create ONE buffer:
     *      LVGL will draw the display's content here and writes it to your display
     *
     * 2. Create TWO buffer:
     *      LVGL will draw the display's content to a buffer and writes it your display.
     *      You should use DMA to write the buffer's content to the display.
     *      It will enable LVGL to draw the next part of the screen to the other buffer while
     *      the data is being sent form the first buffer. It makes rendering and flushing parallel.
     *
     * 3. Double buffering
     *      Set 2 screens sized buffers and set disp_drv.full_refresh = 1.
     *      This way LVGL will always provide the whole rendered screen in `flush_cb`
     *      and you only need to change the frame buffer's address.
     */

	static lv_disp_drv_t disp_drv;                         /*Descriptor of a display driver*/
    lv_disp_drv_init(&disp_drv);                    /*Basic initialization*/

    /*Set up the functions to access to your display*/

    /*Set the resolution of the display*/
    disp_drv.hor_res = LCD_W;
    disp_drv.ver_res = LCD_H;

    /*Used to copy the buffer's content to the display*/
    // disp_flush的函数实现看下面
    disp_drv.flush_cb = disp_flush;

    // 这里是LVGL画面渲染所使用的缓存空间分配，总共有三种方式
    // 你也可以改为malloc分配空间
#define BUFFER_METHOD 2
#if BUFFER_METHOD == 1
    /* Example for 1) */
    static lv_disp_draw_buf_t draw_buf_dsc_1;
    static lv_color_t buf_1[LCD_W * LCD_H / 10];                          /*A buffer for 10 rows*/
    lv_disp_draw_buf_init(&draw_buf_dsc_1, buf_1, NULL, LCD_W * LCD_H / 10);   /*Initialize the display buffer*/
    /*Set a display buffer*/
    disp_drv.draw_buf = &draw_buf_dsc_1;
#elif BUFFER_METHOD == 2
    /* Example for 2) */
    static lv_disp_draw_buf_t draw_buf_dsc_2;
    static lv_color_t buf_2_1[LCD_W * LCD_H / 10];                        /*A buffer for 10 rows*/
    static lv_color_t buf_2_2[LCD_W * LCD_H / 10];                        /*An other buffer for 10 rows*/
    lv_disp_draw_buf_init(&draw_buf_dsc_2, buf_2_1, buf_2_2, LCD_W * LCD_H / 10);   /*Initialize the display buffer*/
    /*Set a display buffer*/
    disp_drv.draw_buf = &draw_buf_dsc_2;
#elif BUFFER_METHOD == 3
    /* Example for 3) also set disp_drv.full_refresh = 1 below*/
    static lv_disp_draw_buf_t draw_buf_dsc_3;
    static lv_color_t buf_3_1[LCD_W * LCD_H];            /*A screen sized buffer*/
    static lv_color_t buf_3_2[LCD_W * LCD_H];            /*An other screen sized buffer*/
    lv_disp_draw_buf_init(&draw_buf_dsc_3, buf_3_1, buf_3_2, LCD_W * LCD_H);   /*Initialize the display buffer*/
    /*Required for Example 3)*/
    disp_drv.full_refresh = 1;
    /*Set a display buffer*/
    disp_drv.draw_buf = &draw_buf_dsc_3;
#endif // 0

    /*-----------------------------------
     * Register the display in LVGL
     *----------------------------------*/

    /* Fill a memory array with a color if you have GPU.
     * Note that, in lv_conf.h you can enable GPUs that has built-in support in LVGL.
     * But if you have a different GPU you can use with this callback.*/
    //disp_drv.gpu_fill_cb = gpu_fill;

    /*Finally register the driver*/
    lv_disp_drv_register(&disp_drv);
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

void lcd_flush_ready_callback(void)
{
    if (g_current_disp_drv != NULL)
    {
        lv_disp_flush_ready(g_current_disp_drv);
        g_current_disp_drv = NULL;
    }
}

/*Initialize your display and the required peripherals.*/
static void disp_init(void)
{
    /*You code here*/
    LCD_Set_Flush_Complete_Callback(lcd_flush_ready_callback);
}

/*Flush the content of the internal buffer the specific area on the display
 *You can use DMA or any hardware acceleration to do this operation in the background but
 *'lv_disp_flush_ready()' has to be called when finished.*/
static void disp_flush(lv_disp_drv_t * disp_drv, const lv_area_t * area, lv_color_t * color_p)
{
    /* LVGL 本身是串行的：上一块区域的 lv_disp_flush_ready() 返回之后才会调用下一次
       flush，所以这里不需要再做"重入保护"。原来的写法在 g_current_disp_drv != NULL
       时又发起一次 LCD_Color_Fill 然后直接 return，既丢了这一帧（HAL_BUSY），
       又永远不调 lv_disp_flush_ready()，导致后续渲染全部堵死。
       现在只关心一件事：这一帧有没有真正发出去，没发出去就立即 ack。 */
    g_current_disp_drv = disp_drv;

    if (LCD_Color_Fill(area->x1, area->y1, area->x2, area->y2, (u16 *)color_p) != HAL_OK)
    {
        /* 驱动层已经等过 SPI 空闲，走到这里说明真的发不出去（超时/硬件异常）：
           清掉待 ack 指针并立即 ack，宁可丢一帧也不能让渲染停摆 */
        g_current_disp_drv = NULL;
        lv_disp_flush_ready(disp_drv);
    }
}


/*OPTIONAL: GPU INTERFACE*/

/*If your MCU has hardware accelerator (GPU) then you can use it to fill a memory with a color*/
//static void gpu_fill(lv_disp_drv_t * disp_drv, lv_color_t * dest_buf, lv_coord_t dest_width,
//                    const lv_area_t * fill_area, lv_color_t color)
//{
//    /*It's an example code which should be done by your GPU*/
//    int32_t x, y;
//    dest_buf += dest_width * fill_area->y1; /*Go to the first line*/
//
//    for(y = fill_area->y1; y <= fill_area->y2; y++) {
//        for(x = fill_area->x1; x <= fill_area->x2; x++) {
//            dest_buf[x] = color;
//        }
//        dest_buf+=dest_width;    /*Go to the next line*/
//    }
//}


#else /*Enable this file at the top*/

/*This dummy typedef exists purely to silence -Wpedantic.*/
typedef int keep_pedantic_happy;
#endif
