///////////////////////////////////////////////////////////////////////////////
//
//                          IMPORTANT NOTICE
//
// The following open source license statement does not apply to any
// entity in the Exception List published by FMSoft.
//
// For more information, please visit:
//
// https://www.fmsoft.cn/exception-list
//
//////////////////////////////////////////////////////////////////////////////
/*
 *   This file is part of MiniGUI, a mature cross-platform windowing
 *   and Graphics User Interface (GUI) support system for embedded systems
 *   and smart IoT devices.
 *
 *   Copyright (C) 2002~2018, Beijing FMSoft Technologies Co., Ltd.
 *   Copyright (C) 1998~2002, WEI Yongming
 *
 *   This program is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *   Or,
 *
 *   As this program is a library, any link to this program must follow
 *   GNU General Public License version 3 (GPLv3). If you cannot accept
 *   GPLv3, you need to be licensed from FMSoft.
 *
 *   If you have got a commercial license of this program, please use it
 *   under the terms and conditions of the commercial license.
 *
 *   For more information about the commercial license, please refer to
 *   <http://www.minigui.com/blog/minigui-licensing-policy/>.
 */
/*
 **  shadow.c: Shadow NEWGAL video driver.
 **    Can be used to provide support for no-access to frame buffer directly.
 **    Can be used to provide support for depth less than 8bpp.
 **    DONOT use this engine with compositing schema.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <error.h>

#include "common.h"

#ifdef _MGGAL_SHADOW

#ifdef _MGRM_PROCESSES

#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <sys/stat.h>

union semun {
    int val;                    /* value for SETVAL */
    struct semid_ds *buf;       /* buffer for IPC_STAT, IPC_SET */
    unsigned short int *array;  /* array for GETALL, SETALL */
    struct seminfo *__buf;      /* buffer for IPC_INFO */
};

#endif

#include "minigui.h"
#include "newgal.h"
#include "sysvideo.h"
#include "pixels_c.h"

#define SHADOWVID_DRIVER_NAME "shadow"
#include "shadow.h"

#define FLAG_REALFB_PREALLOC  0x01
#define _ROT_DIR_CW 0x02
#define _ROT_DIR_CCW 0x04
#define _ROT_DIR_HFLIP 0x08
#define _ROT_DIR_VFLIP 0x10

#define SHADOW_SHM_KEY 0x4D475344
#define SHADOW_SEM_KEY 0x4D475344

#define PALETTE_SIZE 1024

extern void _get_dst_rect_cw (RECT* dst_rect, const RECT* src_rect, RealFBInfo *realfb_info);
extern void _get_dst_rect_hflip (RECT* src_rect, RealFBInfo *realfb_info);
extern void _get_dst_rect_vflip (RECT* src_rect, RealFBInfo *realfb_info);
extern void _get_dst_rect_ccw (RECT* dst_rect, const RECT* src_rect, RealFBInfo *realfb_info);
extern GAL_VideoDevice *GAL_GetVideo(const char* driver_name, BOOL check_compos);

extern void refresh_normal_msb_left (ShadowFBHeader * shadowfb_header, RealFBInfo *realfb_info, void* update);

extern void refresh_cw_msb_left (ShadowFBHeader *shadowfb_header, RealFBInfo *realfb_info, void* update);

extern void refresh_ccw_msb_left (ShadowFBHeader* shadowfb_header, RealFBInfo* realfb_info, void* update);

extern void refresh_hflip_msb_left (ShadowFBHeader* shadowfb_header, RealFBInfo* realfb_info, void* update);

extern void refresh_vflip_msb_left (ShadowFBHeader* shadowfb_header, RealFBInfo* realfb_info, void* update);

extern void refresh_normal_msb_right (ShadowFBHeader * shadowfb_header, RealFBInfo *realfb_info, void* update);

extern void refresh_cw_msb_right (ShadowFBHeader *shadowfb_header, RealFBInfo *realfb_info, void* update);

extern void refresh_ccw_msb_right (ShadowFBHeader* shadowfb_header, RealFBInfo* realfb_info, void* update);

extern void refresh_hflip_msb_right (ShadowFBHeader* shadowfb_header, RealFBInfo* realfb_info, void* update);

extern void refresh_vflip_msb_right (ShadowFBHeader* shadowfb_header, RealFBInfo* realfb_info, void* update);

extern int refresh_init(int pitch);

extern void refresh_destroy(void);

/* Initialization/Query functions */
static int SHADOW_VideoInit (_THIS, GAL_PixelFormat *vformat);
static int RealEngine_SetPalette(RealFBInfo *realfb_info, int firstcolor, int ncolors, void *color);
static GAL_Rect **SHADOW_ListModes (_THIS, GAL_PixelFormat *format, Uint32 flags);
static GAL_Surface *SHADOW_SetVideoMode (_THIS, GAL_Surface *current, int width, int height, int bpp, Uint32 flags);
static int SHADOW_SetColors (_THIS, int firstcolor, int ncolors, GAL_Color *colors);
static void SHADOW_VideoQuit (_THIS);
static BOOL SHADOW_SyncUpdate (_THIS);

/* Hardware surface functions */
static int SHADOW_AllocHWSurface (_THIS, GAL_Surface *surface);
static void SHADOW_FreeHWSurface (_THIS, GAL_Surface *surface);

#ifdef _MGRM_PROCESSES
static int shmid;
/* Down/up a semaphore uninterruptablly. */
static void _sysvipc_sem_op (int semid, int sem_num, int value)
{
    struct sembuf sb;

again:
    sb.sem_num = sem_num;
    sb.sem_op = value;
    sb.sem_flg = 0;

    if (semop (semid, &sb, 1) == -1) {
        if (errno == EINTR) {
            goto again;
        }
    }
}
#endif

static ShadowFBHeader* _shadowfbheader;

DWORD __mg_shadow_rotate_flags;
extern void (*__mg_ial_change_mouse_xy_hook)(int* x, int* y);
extern GAL_Surface* __gal_screen;

/* SHADOW driver bootstrap functions */
static int SHADOW_Available(void)
{
    return 1;
}

static void SHADOW_DeleteDevice(GAL_VideoDevice *device)
{
    free (device->hidden);
    free (device);
    refresh_destroy();
}

static void SHADOW_UpdateRects (_THIS, int numrects, GAL_Rect *rects)
{
    int i;
    RECT bound;

#ifdef _MGRM_PROCESSES
    _sysvipc_sem_op (this->hidden->semid, 0, -1);
#endif
    bound = _shadowfbheader->dirty_rect;

    for (i = 0; i < numrects; i++) {
        RECT rc;
        SetRect (&rc, rects[i].x, rects[i].y,
                rects[i].x + rects[i].w, rects[i].y + rects[i].h);
        if (IsRectEmpty (&bound))
            bound = rc;
        else
            GetBoundRect (&bound, &bound, &rc);
    }

    if (!IsRectEmpty (&bound)) {
        RECT rcScr = GetScreenRect();

        if (IntersectRect (&bound, &bound, &rcScr)) {
            _shadowfbheader->dirty_rect = bound;
            _shadowfbheader->dirty = TRUE;
        }
    }

#ifdef _MGRM_PROCESSES
    _sysvipc_sem_op (this->hidden->semid, 0, 1);
#endif
}

static GAL_VideoDevice *SHADOW_CreateDevice(int devindex)
{
    GAL_VideoDevice *device;

    /* Initialize all variables that we clean on shutdown */
    device = (GAL_VideoDevice *)malloc(sizeof(GAL_VideoDevice));
    if (device) {
        memset (device, 0, (sizeof *device));
        device->hidden = (struct GAL_PrivateVideoData *)
            malloc (sizeof(struct GAL_PrivateVideoData));
    }
    if ( (device == NULL) || (device->hidden == NULL)) {
        GAL_OutOfMemory ();
        if (device) {
            free (device);
        }
        return (0);
    }
    memset (device->hidden, 0, (sizeof *device->hidden));

    /* Set the function pointers */
    device->VideoInit = SHADOW_VideoInit;
    device->ListModes = SHADOW_ListModes;
    device->SetVideoMode = SHADOW_SetVideoMode;
    device->SetColors = SHADOW_SetColors;
    device->VideoQuit = SHADOW_VideoQuit;
    device->AllocHWSurface = SHADOW_AllocHWSurface;
    device->CheckHWBlit = NULL;
    device->FillHWRect = NULL;
    device->SetHWColorKey = NULL;
    device->SetHWAlpha = NULL;
    device->FreeHWSurface = SHADOW_FreeHWSurface;
    device->UpdateRects = SHADOW_UpdateRects;
#ifdef _MGRM_PROCESSES
    if (mgIsServer)
        device->SyncUpdate = SHADOW_SyncUpdate;
    else
        device->SyncUpdate = NULL;
#else
    device->SyncUpdate = SHADOW_SyncUpdate;
#endif

    device->free = SHADOW_DeleteDevice;
    return device;
}

VideoBootStrap SHADOW_bootstrap = {
    SHADOWVID_DRIVER_NAME, "Shadow LCD video driver",
    SHADOW_Available, SHADOW_CreateDevice
};

#define LEN_MODE    32

static ShadowFBOps shadow_fb_ops;
static int RealEngine_GetInfo (RealFBInfo * realfb_info)
{
    GAL_PixelFormat real_vformat;
    char engine[LEN_ENGINE_NAME + 1], mode[LEN_MODE+1],
        rotate_screen[LEN_MODE+1];
    int w, h, depth, pitch_size;
    GAL_VideoDevice* real_device;

    if(realfb_info == NULL)
        return -1;

    if (GetMgEtcValue ("shadow", "real_engine",
                engine, LEN_ENGINE_NAME) < 0) {
        return -1;
    }
    if (GetMgEtcValue ("shadow", "rotate_screen",
                rotate_screen, LEN_MODE) < 0) {
        memset(rotate_screen, 0, sizeof(rotate_screen));
    }

    if (GetMgEtcValue (engine, "defaultmode", mode, LEN_MODE) < 0){
        if (GetMgEtcValue ("system", "defaultmode", mode, LEN_MODE) < 0)
            return -1;
    }

    w = atoi (mode);
    h = atoi (strchr (mode, 'x') + 1);
    depth = atoi (strrchr (mode, '-') + 1);

    memset (&real_vformat, 0, sizeof(real_vformat));
    real_device = GAL_GetVideo (engine, FALSE);

    if (real_device == NULL)
        _ERR_PRINTF ("NEWGAL>SHADOW: can not init real engine (%s) \n", engine);

    realfb_info->real_device = real_device;
    real_device->VideoInit(realfb_info->real_device, &real_vformat);

    real_device->screen = GAL_CreateRGBSurface (GAL_SWSURFACE,
            0, 0, real_vformat.BitsPerPixel, real_vformat.Rmask,
            real_vformat.Gmask, real_vformat.Bmask, real_vformat.Amask);

    if (real_device->screen == NULL)
        _ERR_PRINTF ("NEWGAL>SHADOW: can't create screen of real engine.\n");

    real_device->SetVideoMode(realfb_info->real_device,
            real_device->screen, w, h, depth, GAL_HWPALETTE);

    realfb_info->height = real_device->screen->h;
    realfb_info->width = real_device->screen->w;
    realfb_info->depth = real_device->screen->format->BitsPerPixel;
    realfb_info->fb = real_device->screen->pixels;
    realfb_info->pitch = real_device->screen->pitch;
    realfb_info->flags = 0;

    if (realfb_info->depth <= 8)
        pitch_size = realfb_info->width;
    else
        pitch_size = realfb_info->pitch;

    if (!strncmp(rotate_screen, "cw", sizeof(rotate_screen)))
        realfb_info->flags = _ROT_DIR_CW;
    else if (!strncmp(rotate_screen, "ccw", sizeof(rotate_screen)))
        realfb_info->flags = _ROT_DIR_CCW;
    else if (!strncmp(rotate_screen, "hflip", sizeof(rotate_screen)))
        realfb_info->flags = _ROT_DIR_HFLIP;
    else if (!strncmp(rotate_screen, "vflip", sizeof(rotate_screen)))
        realfb_info->flags = _ROT_DIR_VFLIP;

    __mg_shadow_rotate_flags = realfb_info->flags;

    if (real_device->screen->format->MSBLeft) {
        if (realfb_info->flags & _ROT_DIR_CW)
            shadow_fb_ops.refresh = refresh_cw_msb_left;
        else if (realfb_info->flags & _ROT_DIR_CCW)
            shadow_fb_ops.refresh = refresh_ccw_msb_left;
        else if (realfb_info->flags & _ROT_DIR_HFLIP)
            shadow_fb_ops.refresh = refresh_hflip_msb_left;
        else if(realfb_info->flags & _ROT_DIR_VFLIP)
            shadow_fb_ops.refresh = refresh_vflip_msb_left;
        else
            shadow_fb_ops.refresh = refresh_normal_msb_left;
    }
    else {
        if (realfb_info->flags & _ROT_DIR_CW)
            shadow_fb_ops.refresh = refresh_cw_msb_right;
        else if (realfb_info->flags & _ROT_DIR_CCW)
            shadow_fb_ops.refresh = refresh_ccw_msb_right;
        else if (realfb_info->flags & _ROT_DIR_HFLIP)
            shadow_fb_ops.refresh = refresh_hflip_msb_right;
        else if (realfb_info->flags & _ROT_DIR_VFLIP)
            shadow_fb_ops.refresh = refresh_vflip_msb_right;
        else
            shadow_fb_ops.refresh = refresh_normal_msb_right;
    }

    refresh_init (pitch_size);

    return 0;
}

static int RealEngine_Release(RealFBInfo * realfb_info)
{
    if (realfb_info && realfb_info->real_device) {
        GAL_VideoDevice *video = realfb_info->real_device;
        GAL_VideoDevice *this  = realfb_info->real_device;

        if (realfb_info->flags == FLAG_REALFB_PREALLOC)
            realfb_info->fb = NULL;

        video->VideoQuit (this);
        if (video->physpal)
        {
            free (video->physpal->colors);
            free (video->physpal);
            video->physpal = NULL;
        }

        /* Finish cleaning up video subsystem */
        video->free (this);
    }

    return 0;
}

static int RealEngine_SetPalette(RealFBInfo *realfb_info, int firstcolor,
        int ncolors, void *colors)
{
    GAL_Color* pal_colors;
    pal_colors = (GAL_Color*)((char*)_shadowfbheader + _shadowfbheader->palette_offset);

    if (colors != pal_colors + firstcolor)
        memcpy (pal_colors + firstcolor, colors, ncolors * sizeof(GAL_Color));

    return 1;
}

static int RealEngine_Init(void)
{
    shadow_fb_ops.get_realfb_info = RealEngine_GetInfo;
    shadow_fb_ops.init = RealEngine_Init;
    shadow_fb_ops.release = RealEngine_Release;
    shadow_fb_ops.set_palette = RealEngine_SetPalette;

    return 0;
}

static int SHADOW_VideoInit (_THIS, GAL_PixelFormat *vformat)
{
#ifdef _MGRM_PROCESSES
    if (mgIsServer) {
        union semun sunion;
#endif
        if (RealEngine_Init() < 0) {
            return -1;
        }
        else {
            shadow_fb_ops.init();
        }

#ifdef _MGRM_PROCESSES
        this->hidden->semid = semget (SHADOW_SEM_KEY, 1, 0666 | IPC_CREAT | IPC_EXCL);
        if (this->hidden->semid < 0) {
            perror ("NEWGAL>SHADOW: semget");
            return -1;
        }

        sunion.val = 1;
        semctl (this->hidden->semid, 0, SETVAL, sunion);
    }
    else {
        this->hidden->semid = semget (SHADOW_SEM_KEY, 1, 0);
        if (this->hidden->semid < 0) {
            perror ("NEWGAL>SHADOW: semget");
            return -1;
        }
    }
#endif

    /* We're done! */
    return 0;
}

static BOOL SHADOW_SyncUpdate (_THIS)
{
    RECT dirty_rect;
    GAL_Rect update_rect;

    SetRect(&dirty_rect, 0, 0, _shadowfbheader->width, _shadowfbheader->height);

    if (_shadowfbheader->dirty || _shadowfbheader->palette_changed) {
        GAL_VideoDevice *real_device;
        real_device = this->hidden->realfb_info->real_device;

        if (real_device) {
            if (_shadowfbheader->palette_changed) {
                real_device->SetColors (real_device, _shadowfbheader->firstcolor,
                        _shadowfbheader->ncolors,
                        (GAL_Color*)((char*)_shadowfbheader + _shadowfbheader->palette_offset));
                SetRect (&_shadowfbheader->dirty_rect, 0, 0,
                        _shadowfbheader->width, _shadowfbheader->height);
            }

            shadow_fb_ops.refresh (_shadowfbheader,
                    this->hidden->realfb_info, &(_shadowfbheader->dirty_rect));

            if (this->hidden->realfb_info->flags & _ROT_DIR_CW) {
                _get_dst_rect_cw (&dirty_rect, &(_shadowfbheader->dirty_rect),
                        this->hidden->realfb_info);
            }
            else if (this->hidden->realfb_info->flags & _ROT_DIR_CCW) {
                _get_dst_rect_ccw (&dirty_rect, &(_shadowfbheader->dirty_rect),
                        this->hidden->realfb_info);
            }
            else if (this->hidden->realfb_info->flags & _ROT_DIR_HFLIP) {
                dirty_rect = _shadowfbheader->dirty_rect;
                _get_dst_rect_hflip (&dirty_rect, this->hidden->realfb_info);
            }
            else if (this->hidden->realfb_info->flags & _ROT_DIR_VFLIP) {
                dirty_rect = _shadowfbheader->dirty_rect;
                _get_dst_rect_vflip (&dirty_rect, this->hidden->realfb_info);
            }
            else {
                dirty_rect = _shadowfbheader->dirty_rect;
            }

            update_rect.x = dirty_rect.left;
            update_rect.y = dirty_rect.top;
            update_rect.w = dirty_rect.right - dirty_rect.left;
            update_rect.h = dirty_rect.bottom - dirty_rect.top;

            if (real_device->UpdateRects)
                real_device->UpdateRects(real_device, 1, &update_rect);
            if (real_device->SyncUpdate)
                real_device->SyncUpdate(real_device);
        }

        SetRect (&_shadowfbheader->dirty_rect, 0, 0, 0, 0);
        _shadowfbheader->dirty = FALSE;
        _shadowfbheader->palette_changed = FALSE;
    }

    return TRUE;
}

static void change_mouseXY_cw(int* x, int* y)
{
    int tmp;
    if (*x > (__gal_screen->h-1)) *x = (__gal_screen->h-1);
    if (*y > (__gal_screen->w-1)) *y = (__gal_screen->w-1);
    tmp = *x;
    *x = *y;
    *y = (__gal_screen->h-1) - tmp;
}

static void change_mouseXY_ccw(int* x, int* y)
{
    int tmp;
    if (*x > (__gal_screen->h-1)) *x = (__gal_screen->h-1);
    if (*y > (__gal_screen->w-1)) *y = (__gal_screen->w-1);
    tmp = *y;
    *y = *x;
    *x = (__gal_screen->w-1) - tmp;
}

static void change_mouseXY_hflip(int* x, int* y)
{
    *x = __gal_screen->w - *x;
}

static void change_mouseXY_vflip(int* x, int* y)
{
    *y = __gal_screen->h - *y;
}

#ifndef _MGRM_PROCESSES
/*******************************************/
/*****************Thread   Mode*************/
/*******************************************/
static GAL_Surface *SHADOW_SetVideoMode(_THIS, GAL_Surface *current,
        int width, int height, int bpp, Uint32 flags)
{
    int size;
    int ret;
    RealFBInfo * realfb_info;
    ShadowFBHeader shadowfbheader;
    GAL_VideoDevice* real_device;

    size = 0;
    ret = 0;

    bzero(&shadowfbheader, sizeof(shadowfbheader));
    realfb_info = malloc (sizeof(RealFBInfo));
    if (shadow_fb_ops.get_realfb_info (realfb_info))
    {
        _ERR_PRINTF ("NEWGAL>SHADOW: "
                "Couldn't get the real engine information\n");
        return NULL;
    }

    real_device = realfb_info->real_device;
    if (realfb_info->depth <= 8)
        shadowfbheader.depth = 8;
    else
        shadowfbheader.depth = realfb_info->depth;

    if (realfb_info->flags & _ROT_DIR_CW) {
        __mg_ial_change_mouse_xy_hook = change_mouseXY_cw;
    }
    else if (realfb_info->flags & _ROT_DIR_CCW) {
        __mg_ial_change_mouse_xy_hook = change_mouseXY_ccw;
    }
    else if (realfb_info->flags & _ROT_DIR_HFLIP) {
        __mg_ial_change_mouse_xy_hook = change_mouseXY_hflip;
    }
    else if (realfb_info->flags & _ROT_DIR_VFLIP) {
        __mg_ial_change_mouse_xy_hook = change_mouseXY_vflip;
    }

    if((realfb_info->flags & _ROT_DIR_CW) ||
            (realfb_info->flags & _ROT_DIR_CCW)) {
        shadowfbheader.width = realfb_info->height;
        shadowfbheader.height = realfb_info->width;
    }
    else {
        shadowfbheader.width = realfb_info->width;
        shadowfbheader.height = realfb_info->height;
    }

    shadowfbheader.pitch = ((shadowfbheader.width * shadowfbheader.depth) + 31) / 32*4;

    if (!(realfb_info->flags & FLAG_REALFB_PREALLOC)) {
        size = shadowfbheader.pitch * shadowfbheader.height;
    }

    if (shadowfbheader.depth <= 8)
        size += PALETTE_SIZE;

    size += sizeof (ShadowFBHeader);
    _shadowfbheader = (ShadowFBHeader *) malloc (size);
    memcpy(_shadowfbheader, &shadowfbheader, sizeof(ShadowFBHeader));

    /* INIT  Share Memory  ShadowFBHeader */
    this->hidden->realfb_info = realfb_info;
    if ((realfb_info->real_device))
        this->hidden->realfb_info->fb = real_device->screen->pixels;
    _shadowfbheader->info_size = size;

    _shadowfbheader->dirty = FALSE;
    SetRect (&(_shadowfbheader->dirty_rect), 0, 0, 0, 0);
    _shadowfbheader->palette_changed = FALSE;
    _shadowfbheader->palette_offset = sizeof(ShadowFBHeader);

    if(shadowfbheader.depth <= 8)
        _shadowfbheader->fb_offset = _shadowfbheader->palette_offset+PALETTE_SIZE;
    else
        _shadowfbheader->fb_offset = _shadowfbheader->palette_offset;

    if ((realfb_info->real_device))
        GAL_SetClipRect (real_device->screen, NULL);

    if (!GAL_ReallocFormat (current, _shadowfbheader->depth,
                real_device->screen->format->Rmask,
                real_device->screen->format->Gmask,
                real_device->screen->format->Bmask,
                real_device->screen->format->Amask)) {
        if (shadow_fb_ops.release)
            shadow_fb_ops.release (realfb_info);
        _ERR_PRINTF ("NEWGAL>SHADOW: "
                "Couldn't allocate new pixel format for requested mode");
        return (NULL);
    }

    /* Set up the new mode framebuffer */
    current->flags = GAL_HWSURFACE | GAL_FULLSCREEN;
    current->w = _shadowfbheader->width;
    current->h = _shadowfbheader->height;
    current->pitch = _shadowfbheader->pitch;
    current->pixels = (char *)_shadowfbheader + _shadowfbheader->fb_offset;

    /* We're done */
    return (current);
}

static void SHADOW_VideoQuit (_THIS)
{
    if (_shadowfbheader) {
        _shadowfbheader->dirty = FALSE;
        free(_shadowfbheader);
        _shadowfbheader = NULL;
    }

    if (this->hidden->realfb_info) {
        shadow_fb_ops.release(this->hidden->realfb_info);
        this->hidden->realfb_info = NULL;
    }
}

#else

/*******************************************/
/*****************Proc   Mode***************/
/*******************************************/

static GAL_Surface *SHADOW_SetVideoMode(_THIS, GAL_Surface *current,
        int width, int height, int bpp, Uint32 flags)
{
    int ret, size;

    RealFBInfo * realfb_info;
    GAL_VideoDevice* real_device = NULL;
    realfb_info = NULL;
    shmid = 0;
    size = 0;
    ret = 0;

    if (mgIsServer) {
        ShadowFBHeader shadowfbheader;

        realfb_info = malloc (sizeof(RealFBInfo));
        if (shadow_fb_ops.get_realfb_info (realfb_info)){
            _ERR_PRINTF ("NEWGAL>SHADOW: "
                    "Couldn't get the real engine information\n");
        }
        if (realfb_info->real_device)
            real_device = realfb_info->real_device;

        if (!(realfb_info->flags & FLAG_REALFB_PREALLOC)) {
            if (realfb_info->depth <= 8)
                shadowfbheader.depth = 8;
            else
                shadowfbheader.depth = realfb_info->depth;
        }
        else {
            _ERR_PRINTF ("NEWGAL>SHADOW: "
                    "FLAG_REALFB_PREALLOC is TRUE, Can't run in proc mode!");
            return NULL;
        }

        if (realfb_info->flags & _ROT_DIR_CW) {
            __mg_ial_change_mouse_xy_hook = change_mouseXY_cw;
        }
        else if (realfb_info->flags & _ROT_DIR_CCW) {
            __mg_ial_change_mouse_xy_hook = change_mouseXY_ccw;
        }
        else if (realfb_info->flags & _ROT_DIR_HFLIP) {
            __mg_ial_change_mouse_xy_hook = change_mouseXY_hflip;
        }
        else if (realfb_info->flags & _ROT_DIR_VFLIP) {
            __mg_ial_change_mouse_xy_hook = change_mouseXY_vflip;
        }

        if((realfb_info->flags & _ROT_DIR_CW) ||
                (realfb_info->flags & _ROT_DIR_CCW)) {
            shadowfbheader.width = realfb_info->height;
            shadowfbheader.height = realfb_info->width;
        }
        else {
            shadowfbheader.width = realfb_info->width;
            shadowfbheader.height = realfb_info->height;
        }

        shadowfbheader.pitch = ((shadowfbheader.width * shadowfbheader.depth) + 31) / 32*4;

        size = shadowfbheader.pitch * shadowfbheader.height;
        if (shadowfbheader.depth <= 8)
            size += PALETTE_SIZE;
        size += sizeof (ShadowFBHeader);

        shmid = shmget (SHADOW_SHM_KEY, size, IPC_CREAT | 0666 | IPC_EXCL);
        if (shmid == -1){
            perror("NEWGAL>SHADOW: shmget");
            shadow_fb_ops.release(realfb_info);
            return NULL;
        }

        _shadowfbheader = (ShadowFBHeader *) shmat (shmid, 0, 0);
        memcpy (_shadowfbheader, &shadowfbheader, sizeof (ShadowFBHeader));

        this->hidden->realfb_info = realfb_info;
        if ((realfb_info->real_device))
            this->hidden->realfb_info->fb = real_device->screen->pixels;

        /* INIT  Share Memory  ShadowFBHeader */
        _shadowfbheader->info_size = size;
        _shadowfbheader->dirty = FALSE;
        SetRect (&(_shadowfbheader->dirty_rect), 0, 0, 0, 0);
        _shadowfbheader->palette_changed = FALSE;
        _shadowfbheader->palette_offset = sizeof(ShadowFBHeader);

        if(shadowfbheader.depth <= 8)
            _shadowfbheader->fb_offset = _shadowfbheader->palette_offset + PALETTE_SIZE;
        else
            _shadowfbheader->fb_offset = _shadowfbheader->palette_offset;

        _shadowfbheader->Rmask = real_device->screen->format->Rmask;
        _shadowfbheader->Gmask = real_device->screen->format->Gmask;
        _shadowfbheader->Bmask = real_device->screen->format->Bmask;
        _shadowfbheader->Amask = real_device->screen->format->Amask;

        if ((realfb_info->real_device))
            GAL_SetClipRect (real_device->screen, NULL);
    }
    else {
        shmid = shmget(SHADOW_SHM_KEY, 0, 0);
        _shadowfbheader = (ShadowFBHeader *) shmat (shmid, 0, 0);
    }

    /* Allocate the new pixel format for the screen */
    if (!GAL_ReallocFormat (current, _shadowfbheader->depth,
                _shadowfbheader->Rmask, _shadowfbheader->Gmask,
                _shadowfbheader->Bmask, _shadowfbheader->Amask)){
        if (shadow_fb_ops.release)
            shadow_fb_ops.release (realfb_info);

        _ERR_PRINTF ("NEWGAL>SHADOW: "
                "Couldn't allocate new pixel format for requested mode");
        return (NULL);
    }

    /* Set up the new mode framebuffer */

    current->flags = GAL_HWSURFACE | GAL_FULLSCREEN;
    current->w = _shadowfbheader->width;
    current->h = _shadowfbheader->height;
    current->pitch = _shadowfbheader->pitch;
    current->pixels = (char *)(_shadowfbheader) + _shadowfbheader->fb_offset;

    /* We're done */
    return (current);
}

static void SHADOW_VideoQuit (_THIS)
{
    ShadowFBHeader* tmp;
    union semun ignored;

    if (!mgIsServer) {
        _shadowfbheader->dirty = FALSE;
        if (this->screen->pixels) {
            shmdt(_shadowfbheader);
            this->screen->pixels = NULL;
        }
    }
    else {
        tmp = _shadowfbheader;
        _shadowfbheader = NULL;
        shmdt (tmp);
        shadow_fb_ops.release(this->hidden->realfb_info);

        if (shmctl (shmid, IPC_RMID, NULL))
            perror ("NEWGAL>SHADOW: shmctl");

        if (semctl (this->hidden->semid, 0, IPC_RMID, ignored) < 0)
            perror ("NEWGAL>SHADOW: smmctl");
    }
}

#endif

static GAL_Rect **SHADOW_ListModes (_THIS, GAL_PixelFormat *format,
        Uint32 flags)
{
    return (GAL_Rect **) -1;
}

/* We don't actually allow hardware surfaces other than the main one */
static int SHADOW_AllocHWSurface (_THIS, GAL_Surface *surface)
{
    return -1;
}

static void SHADOW_FreeHWSurface (_THIS, GAL_Surface *surface)
{
    surface->pixels = NULL;
}

static int SHADOW_SetColors (_THIS, int firstcolor, int ncolors,
        GAL_Color *colors)
{
    _shadowfbheader->firstcolor = firstcolor;
    _shadowfbheader->ncolors = ncolors;
#ifdef _MGRM_PROCESSES
    if (mgIsServer) {
#endif
        if ((shadow_fb_ops.set_palette != NULL)
                && (_shadowfbheader->depth <= 8)){
            shadow_fb_ops.set_palette(this->hidden->realfb_info,
                    firstcolor, ncolors, colors);
            _shadowfbheader->palette_changed = TRUE;
        }

        return 0;
#ifdef _MGRM_PROCESSES
    }
    else {
#endif
        if (_shadowfbheader->depth <= 8){
            RealEngine_SetPalette(this->hidden->realfb_info, firstcolor,
                    ncolors, colors);
            _shadowfbheader->palette_changed = TRUE;
        }
#ifdef _MGRM_PROCESSES
    }
#endif

    return 0;
}

#endif /* _MGGAL_SHADOW */

