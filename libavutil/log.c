/*
 * log functions
 * Copyright (c) 2003 Michel Bardiaux
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 * logging functions
 */

#include "config.h"

#if HAVE_UNISTD_H
#include <unistd.h>
#endif
#if HAVE_IO_H
#include <io.h>
#endif
#include <stdarg.h>
#include <stdlib.h>
#include "avutil.h"
#include "bprint.h"
#include "common.h"
#include "internal.h"
#include "log.h"
#include "libavutil/application.h"

#if HAVE_PTHREADS
#include <pthread.h>
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
#endif

#define LINE_SZ 1024

#if HAVE_VALGRIND_VALGRIND_H
#include <valgrind/valgrind.h>
/* this is the log level at which valgrind will output a full backtrace */
#define BACKTRACE_LOGLEVEL AV_LOG_ERROR
#endif

static int av_log_level = AV_LOG_INFO;
static int av_report_log_level = AV_REPORT_LOG_MAIN;
static int flags;


#define NB_LEVELS 8
#if defined(_WIN32) && !defined(__MINGW32CE__) && HAVE_SETCONSOLETEXTATTRIBUTE
#include <windows.h>
static const uint8_t color[16 + AV_CLASS_CATEGORY_NB] = {
    [AV_LOG_PANIC  /8] = 12,
    [AV_LOG_FATAL  /8] = 12,
    [AV_LOG_ERROR  /8] = 12,
    [AV_LOG_WARNING/8] = 14,
    [AV_LOG_INFO   /8] =  7,
    [AV_LOG_VERBOSE/8] = 10,
    [AV_LOG_DEBUG  /8] = 10,
    [AV_LOG_TRACE  /8] = 8,
    [16+AV_CLASS_CATEGORY_NA              ] =  7,
    [16+AV_CLASS_CATEGORY_INPUT           ] = 13,
    [16+AV_CLASS_CATEGORY_OUTPUT          ] =  5,
    [16+AV_CLASS_CATEGORY_MUXER           ] = 13,
    [16+AV_CLASS_CATEGORY_DEMUXER         ] =  5,
    [16+AV_CLASS_CATEGORY_ENCODER         ] = 11,
    [16+AV_CLASS_CATEGORY_DECODER         ] =  3,
    [16+AV_CLASS_CATEGORY_FILTER          ] = 10,
    [16+AV_CLASS_CATEGORY_BITSTREAM_FILTER] =  9,
    [16+AV_CLASS_CATEGORY_SWSCALER        ] =  7,
    [16+AV_CLASS_CATEGORY_SWRESAMPLER     ] =  7,
    [16+AV_CLASS_CATEGORY_DEVICE_VIDEO_OUTPUT ] = 13,
    [16+AV_CLASS_CATEGORY_DEVICE_VIDEO_INPUT  ] = 5,
    [16+AV_CLASS_CATEGORY_DEVICE_AUDIO_OUTPUT ] = 13,
    [16+AV_CLASS_CATEGORY_DEVICE_AUDIO_INPUT  ] = 5,
    [16+AV_CLASS_CATEGORY_DEVICE_OUTPUT       ] = 13,
    [16+AV_CLASS_CATEGORY_DEVICE_INPUT        ] = 5,
};

static int16_t background, attr_orig;
static HANDLE con;
#else

static const uint32_t color[16 + AV_CLASS_CATEGORY_NB] = {
    [AV_LOG_PANIC  /8] =  52 << 16 | 196 << 8 | 0x41,
    [AV_LOG_FATAL  /8] = 208 <<  8 | 0x41,
    [AV_LOG_ERROR  /8] = 196 <<  8 | 0x11,
    [AV_LOG_WARNING/8] = 226 <<  8 | 0x03,
    [AV_LOG_INFO   /8] = 253 <<  8 | 0x09,
    [AV_LOG_VERBOSE/8] =  40 <<  8 | 0x02,
    [AV_LOG_DEBUG  /8] =  34 <<  8 | 0x02,
    [AV_LOG_TRACE  /8] =  34 <<  8 | 0x07,
    [16+AV_CLASS_CATEGORY_NA              ] = 250 << 8 | 0x09,
    [16+AV_CLASS_CATEGORY_INPUT           ] = 219 << 8 | 0x15,
    [16+AV_CLASS_CATEGORY_OUTPUT          ] = 201 << 8 | 0x05,
    [16+AV_CLASS_CATEGORY_MUXER           ] = 213 << 8 | 0x15,
    [16+AV_CLASS_CATEGORY_DEMUXER         ] = 207 << 8 | 0x05,
    [16+AV_CLASS_CATEGORY_ENCODER         ] =  51 << 8 | 0x16,
    [16+AV_CLASS_CATEGORY_DECODER         ] =  39 << 8 | 0x06,
    [16+AV_CLASS_CATEGORY_FILTER          ] = 155 << 8 | 0x12,
    [16+AV_CLASS_CATEGORY_BITSTREAM_FILTER] = 192 << 8 | 0x14,
    [16+AV_CLASS_CATEGORY_SWSCALER        ] = 153 << 8 | 0x14,
    [16+AV_CLASS_CATEGORY_SWRESAMPLER     ] = 147 << 8 | 0x14,
    [16+AV_CLASS_CATEGORY_DEVICE_VIDEO_OUTPUT ] = 213 << 8 | 0x15,
    [16+AV_CLASS_CATEGORY_DEVICE_VIDEO_INPUT  ] = 207 << 8 | 0x05,
    [16+AV_CLASS_CATEGORY_DEVICE_AUDIO_OUTPUT ] = 213 << 8 | 0x15,
    [16+AV_CLASS_CATEGORY_DEVICE_AUDIO_INPUT  ] = 207 << 8 | 0x05,
    [16+AV_CLASS_CATEGORY_DEVICE_OUTPUT       ] = 213 << 8 | 0x15,
    [16+AV_CLASS_CATEGORY_DEVICE_INPUT        ] = 207 << 8 | 0x05,
};

#endif
static int use_color = -1;

#define TCP_CONNECTION_LOG_SIZE  256

static void check_color_terminal(void)
{
#if defined(_WIN32) && !defined(__MINGW32CE__) && HAVE_SETCONSOLETEXTATTRIBUTE
    CONSOLE_SCREEN_BUFFER_INFO con_info;
    con = GetStdHandle(STD_ERROR_HANDLE);
    use_color = (con != INVALID_HANDLE_VALUE) && !getenv("NO_COLOR") &&
                !getenv("AV_LOG_FORCE_NOCOLOR");
    if (use_color) {
        GetConsoleScreenBufferInfo(con, &con_info);
        attr_orig  = con_info.wAttributes;
        background = attr_orig & 0xF0;
    }
#elif HAVE_ISATTY
    char *term = getenv("TERM");
    use_color = !getenv("NO_COLOR") && !getenv("AV_LOG_FORCE_NOCOLOR") &&
                (getenv("TERM") && isatty(2) || getenv("AV_LOG_FORCE_COLOR"));
    if (   getenv("AV_LOG_FORCE_256COLOR")
        || (term && strstr(term, "256color")))
        use_color *= 256;
#else
    use_color = getenv("AV_LOG_FORCE_COLOR") && !getenv("NO_COLOR") &&
               !getenv("AV_LOG_FORCE_NOCOLOR");
#endif
}

static void colored_fputs(int level, int tint, const char *str)
{
    int local_use_color;
    if (!*str)
        return;

    if (use_color < 0)
        check_color_terminal();

    if (level == AV_LOG_INFO/8) local_use_color = 0;
    else                        local_use_color = use_color;

#if defined(_WIN32) && !defined(__MINGW32CE__) && HAVE_SETCONSOLETEXTATTRIBUTE
    if (local_use_color)
        SetConsoleTextAttribute(con, background | color[level]);
    fputs(str, stderr);
    if (local_use_color)
        SetConsoleTextAttribute(con, attr_orig);
#else
    if (local_use_color == 1) {
        fprintf(stderr,
                "\033[%d;3%dm%s\033[0m",
                (color[level] >> 4) & 15,
                color[level] & 15,
                str);
    } else if (tint && use_color == 256) {
        fprintf(stderr,
                "\033[48;5;%dm\033[38;5;%dm%s\033[0m",
                (color[level] >> 16) & 0xff,
                tint,
                str);
    } else if (local_use_color == 256) {
        fprintf(stderr,
                "\033[48;5;%dm\033[38;5;%dm%s\033[0m",
                (color[level] >> 16) & 0xff,
                (color[level] >> 8) & 0xff,
                str);
    } else
        fputs(str, stderr);
#endif

}

const char *av_default_item_name(void *ptr)
{
    return (*(AVClass **) ptr)->class_name;
}

AVClassCategory av_default_get_category(void *ptr)
{
    return (*(AVClass **) ptr)->category;
}

static void sanitize(uint8_t *line){
    while(*line){
        if(*line < 0x08 || (*line > 0x0D && *line < 0x20))
            *line='?';
        line++;
    }
}

static int get_category(void *ptr){
    AVClass *avc = *(AVClass **) ptr;
    if(    !avc
        || (avc->version&0xFF)<100
        ||  avc->version < (51 << 16 | 59 << 8)
        ||  avc->category >= AV_CLASS_CATEGORY_NB) return AV_CLASS_CATEGORY_NA + 16;

    if(avc->get_category)
        return avc->get_category(ptr) + 16;

    return avc->category + 16;
}

static const char *get_level_str(int level)
{
    switch (level) {
    case AV_LOG_QUIET:
        return "quiet";
    case AV_LOG_DEBUG:
        return "debug";
    case AV_LOG_VERBOSE:
        return "verbose";
    case AV_LOG_INFO:
        return "info";
    case AV_LOG_WARNING:
        return "warning";
    case AV_LOG_ERROR:
        return "error";
    case AV_LOG_FATAL:
        return "fatal";
    case AV_LOG_PANIC:
        return "panic";
    default:
        return "";
    }
}

static void format_line(void *avcl, int level, const char *fmt, va_list vl,
                        AVBPrint part[4], int *print_prefix, int type[2])
{
    AVClass* avc = avcl ? *(AVClass **) avcl : NULL;
    av_bprint_init(part+0, 0, 1);
    av_bprint_init(part+1, 0, 1);
    av_bprint_init(part+2, 0, 1);
    av_bprint_init(part+3, 0, 65536);

    if(type) type[0] = type[1] = AV_CLASS_CATEGORY_NA + 16;
    if (*print_prefix && avc) {
        if (avc->parent_log_context_offset) {
            AVClass** parent = *(AVClass ***) (((uint8_t *) avcl) +
                                   avc->parent_log_context_offset);
            if (parent && *parent) {
                av_bprintf(part+0, "[%s @ %p] ",
                         (*parent)->item_name(parent), parent);
                if(type) type[0] = get_category(parent);
            }
        }
        av_bprintf(part+1, "[%s @ %p] ",
                 avc->item_name(avcl), avcl);
        if(type) type[1] = get_category(avcl);

        if (flags & AV_LOG_PRINT_LEVEL)
            av_bprintf(part+2, "[%s] ", get_level_str(level));
    }

    av_vbprintf(part+3, fmt, vl);

    if(*part[0].str || *part[1].str || *part[2].str || *part[3].str) {
        char lastc = part[3].len && part[3].len <= part[3].size ? part[3].str[part[3].len - 1] : 0;
        *print_prefix = lastc == '\n' || lastc == '\r';
    }
}

void av_log_format_line(void *ptr, int level, const char *fmt, va_list vl,
                        char *line, int line_size, int *print_prefix)
{
    av_log_format_line2(ptr, level, fmt, vl, line, line_size, print_prefix);
}

int av_log_format_line2(void *ptr, int level, const char *fmt, va_list vl,
                        char *line, int line_size, int *print_prefix)
{
    AVBPrint part[4];
    int ret;

    format_line(ptr, level, fmt, vl, part, print_prefix, NULL);
    ret = snprintf(line, line_size, "%s%s%s%s", part[0].str, part[1].str, part[2].str, part[3].str);
    av_bprint_finalize(part+3, NULL);
    return ret;
}

void av_log_default_callback(void* ptr, int level, const char* fmt, va_list vl)
{
    static int print_prefix = 1;
    static int count;
    static char prev[LINE_SZ];
    AVBPrint part[4];
    char line[LINE_SZ];
    static int is_atty;
    int type[2];
    unsigned tint = 0;

    if (level >= 0) {
        tint = level & 0xff00;
        level &= 0xff;
    }

    if (level > av_log_level)
        return;
#if HAVE_PTHREADS
    pthread_mutex_lock(&mutex);
#endif

    format_line(ptr, level, fmt, vl, part, &print_prefix, type);
    snprintf(line, sizeof(line), "%s%s%s%s", part[0].str, part[1].str, part[2].str, part[3].str);

#if HAVE_ISATTY
    if (!is_atty)
        is_atty = isatty(2) ? 1 : -1;
#endif

    if (print_prefix && (flags & AV_LOG_SKIP_REPEATED) && !strcmp(line, prev) &&
        *line && line[strlen(line) - 1] != '\r'){
        count++;
        if (is_atty == 1)
            fprintf(stderr, "    Last message repeated %d times\r", count);
        goto end;
    }
    if (count > 0) {
        fprintf(stderr, "    Last message repeated %d times\n", count);
        count = 0;
    }
    strcpy(prev, line);
    sanitize(part[0].str);
    colored_fputs(type[0], 0, part[0].str);
    sanitize(part[1].str);
    colored_fputs(type[1], 0, part[1].str);
    sanitize(part[2].str);
    colored_fputs(av_clip(level >> 3, 0, NB_LEVELS - 1), tint >> 8, part[2].str);
    sanitize(part[3].str);
    colored_fputs(av_clip(level >> 3, 0, NB_LEVELS - 1), tint >> 8, part[3].str);

#if CONFIG_VALGRIND_BACKTRACE
    if (level <= BACKTRACE_LOGLEVEL)
        VALGRIND_PRINTF_BACKTRACE("%s", "");
#endif
end:
    av_bprint_finalize(part+3, NULL);
#if HAVE_PTHREADS
    pthread_mutex_unlock(&mutex);
#endif
}

static void (*av_log_callback)(void*, int, const char*, va_list) =
    av_log_default_callback;
//APP to do
static void (*star_log_callback)(void*, int, const char*, va_list) =
    av_log_default_callback;
///////////////////////////////////
void av_log(void* avcl, int level, const char *fmt, ...)
{
    AVClass* avc = avcl ? *(AVClass **) avcl : NULL;
    va_list vl;
    va_start(vl, fmt);
    if (avc && avc->version >= (50 << 16 | 15 << 8 | 2) &&
        avc->log_level_offset_offset && level >= AV_LOG_FATAL)
        level += *(int *) (((uint8_t *) avcl) + avc->log_level_offset_offset);
    av_vlog(avcl, level, fmt, vl);
    va_end(vl);
}
//APP to do
void ffPlayer_error_log(void* app_ctx, int level, const char *fmt, ...)
{
    /*AVClass* avc = avcl ? *(AVClass **) avcl : NULL;
    va_list vl;
    int av_level=AV_LOG_INFO;
    va_start(vl, fmt);
    if (avc && avc->version >= (50 << 16 | 15 << 8 | 2) &&
        avc->log_level_offset_offset && av_level >= AV_LOG_FATAL)
        av_level += *(int *) (((uint8_t *) avcl) + avc->log_level_offset_offset);*/
//    char new_string[1024]="FFPLAYER_START_ERROR_LOG:";
//    switch(level){
//    case 0:
//    	strcat(new_string,"0");
//    	break;
//    case 1:
//        strcat(new_string,"1");
//        break;
//    }
    AVApplicationContext *ctx = (AVApplicationContext *)app_ctx;
    va_list vl;
    int av_level=AV_LOG_INFO;
    va_start(vl, fmt);
    
    char new_string[LINE_SZ]={0};
    sprintf( new_string, "FFPLAYER_START_ERROR_LOG:%d", level);
    
    char count_string[32]={0};
    //sprintf(count_string,":%d:",ctx->lss->start_error_log_count);
	sprintf(count_string,":%d:",0);
    strcat(new_string,count_string);
    //ctx->lss->start_error_log_count++;
    strcat(new_string,fmt);
    ffPlayer_vlog(app_ctx, av_level, new_string, vl);
    va_end(vl);
}

void ffPlayer_start_log(void* app_ctx, int level, const char *fmt, ...)
{
    if(av_report_log_level==AV_REPORT_LOG_MAIN)
    {
           if(level==FFPLAYER_TIME_LOG_TCP || level==FFPLAYER_TIME_LOG_FLOW_LOG) return;
    }else if(av_report_log_level=AV_REPORT_LOG_LOW)
    {
        if(level==FFPLAYER_TIME_LOG_TCP || level==FFPLAYER_TIME_LOG_FLOW_LOG) return;
    }
    
    AVApplicationContext *ctx = (AVApplicationContext *)app_ctx;
    va_list vl;
    int av_level=AV_LOG_INFO;
    va_start(vl, fmt);
    char new_string[LINE_SZ]={0};
    sprintf( new_string, "FFPLAYER_START_TIME_LOG:%d", level);
    char count_string[32]={0};
    strcat(new_string,":0:");
    strcat(new_string,fmt);
    ffPlayer_vlog(app_ctx, av_level, new_string, vl);
    va_end(vl);
}

void ffPlayer_play_log(void* app_ctx, int level, const char*tag, const char *fmt, ...)
{
    if(av_report_log_level==AV_REPORT_LOG_MAIN)
    {
        if(level==FFPLAYER_TIME_LOG_TCP || level==FFPLAYER_TIME_LOG_FLOW_LOG) return;
    }else if(av_report_log_level=AV_REPORT_LOG_LOW)
    {
        if(level==FFPLAYER_TIME_LOG_TCP || level==FFPLAYER_TIME_LOG_FLOW_LOG) return;
    }
    
    va_list vl;
    int av_level=AV_LOG_INFO;
    va_start(vl, fmt);
    
    if (level == FFPLAYER_TIME_LOG_FLOW_LOG) {
        ffPlayer_vlog(app_ctx, av_level, fmt, vl);
        va_end(vl);
    }
    else
    {
        char new_string[LINE_SZ]={0};
        sprintf( new_string, "%s:%d:", tag, level);
        strcat(new_string,fmt);
        ffPlayer_vlog(app_ctx, av_level, new_string, vl);
        va_end(vl);
    }
}


////////////////////
void av_vlog(void* avcl, int level, const char *fmt, va_list vl)
{
    void (*log_callback)(void*, int, const char*, va_list) = av_log_callback;
    if (log_callback)
        log_callback(avcl, level, fmt, vl);
}
//APP to do
void ffPlayer_vlog(void* app_ctx, int level, const char *fmt, va_list vl)
{
    void (*log_callback)(void*, int, const char*, va_list) = star_log_callback;
    if (log_callback)
        log_callback(app_ctx, level, fmt, vl);
}
//////////////////////////////////////
int av_log_get_level(void)
{
    return av_log_level;
}
int av_report_log_get_level(void)
{
    return av_report_log_level;
}

void av_log_set_level(int level)
{
    av_log_level = level;
}
void av_report_log_set_level(int level)
{
    av_report_log_level = level;
}

void av_log_set_flags(int arg)
{
    flags = arg;
}

int av_log_get_flags(void)
{
    return flags;
}

void av_log_set_callback(void (*callback)(void*, int, const char*, va_list))
{
    av_log_callback = callback;
}
//APP to do
void ffPlayer_log_set_callback(void (*callback)(void*, int, const char*, va_list))
{
    star_log_callback = callback;
}
//////////////////////////////
static void missing_feature_sample(int sample, void *avc, const char *msg,
                                   va_list argument_list)
{
    av_vlog(avc, AV_LOG_WARNING, msg, argument_list);
    av_log(avc, AV_LOG_WARNING, " is not implemented. Update your FFmpeg "
           "version to the newest one from Git. If the problem still "
           "occurs, it means that your file has a feature which has not "
           "been implemented.\n");
    if (sample)
        av_log(avc, AV_LOG_WARNING, "If you want to help, upload a sample "
               "of this file to ftp://upload.ffmpeg.org/incoming/ "
               "and contact the ffmpeg-devel mailing list. (ffmpeg-devel@ffmpeg.org)\n");
}

void avpriv_request_sample(void *avc, const char *msg, ...)
{
    va_list argument_list;

    va_start(argument_list, msg);
    missing_feature_sample(1, avc, msg, argument_list);
    va_end(argument_list);
}

void avpriv_report_missing_feature(void *avc, const char *msg, ...)
{
    va_list argument_list;

    va_start(argument_list, msg);
    missing_feature_sample(0, avc, msg, argument_list);
    va_end(argument_list);
}

void add_flow_log_str(void *app_ctx, void *uss, enum FLOW_LOG type, const char *data)
{
    AVApplicationContext* ctx = (AVApplicationContext *)app_ctx;
	URLStartStatus* puss = (URLStartStatus*)uss;
	
    if (!ctx || !puss)
        return;
	
	FlowLogStatus *pfls = puss->fls;
    
    memset(pfls->flow_log_info,0,FLOW_LOG_SIZE);
    if (type==FL_FILE)
    {
        char* pfile = strrchr(data, '/');
        if (pfile==NULL) {
            return;
        }
        
        if (strlen(pfile+1) > (FLOW_LOG_SIZE - 10))
        {
            av_log(NULL, AV_LOG_WARNING,"add_flow_log_str:FL_FILE is longer than 1024\n");
            pfls->flow_log_need_send=0;
            return;
        }
        char ctemp[1024];
        sprintf(ctemp,"\"file\":\"%s\"",pfile+1);
        strcat(pfls->flow_log_info,ctemp);
        pfls->flow_log_need_send=1;
    }
}

static int add_flow_log_combine(FlowLogStatus *pfls,const char *ctemp)
{
    if ((strlen(pfls->flow_log_info) + strlen(ctemp)) < (FLOW_LOG_SIZE - strlen(LOG_TAG_PLAY_FLOW_LOG) -3))  //3 is include '\0' ',' '='
    {
        //sprintf(flow_log_info,"%s,%s",flow_log_info,ctemp);
        strcat(pfls->flow_log_info,",");
        strcat(pfls->flow_log_info,ctemp);
        return 1;
    }
    else
    {
        av_log(NULL, AV_LOG_WARNING,"add_flow_log_combine: flow log size over flow\n");
        pfls->flow_log_need_send=0;
        return 0;
    }
}

void add_flow_log_string(void *app_ctx, void *uss, enum FLOW_LOG type, const char *data){
    AVApplicationContext *ctx = (AVApplicationContext *)app_ctx;
    URLStartStatus* puss = (URLStartStatus*)uss;
	
    if (!ctx || !puss)
        return;
	
	FlowLogStatus *pfls = puss->fls;
    
    if (data==NULL||strlen(data)==0) {
        return;
    }
	
    char ctemp[FLOW_LOG_SIZE]={0};
    switch (type) {
        case FL_TCP_CONNECTIONS:
            sprintf(ctemp,"\"tcp_connect_logs\":[%s]",data);
            add_flow_log_combine(pfls,ctemp);
            break;
    }
}

void add_flow_log(void *app_ctx, void *uss, enum FLOW_LOG type, int64_t data)
{
    AVApplicationContext *ctx = (AVApplicationContext *)app_ctx;
    URLStartStatus* puss = (URLStartStatus*)uss;
	
    if (!ctx || !puss)
        return;
	
	FlowLogStatus *pfls = puss->fls;
    
    char ctemp[1024];
    switch (type) {
        case FL_FILE_SIZE:
            sprintf(ctemp,"\"file_size\":%lld",data);
            add_flow_log_combine(pfls,ctemp);
            break;
        case FL_DNS_USE_PRE:
            sprintf(ctemp,"\"dns_use_pre\":%lld",data);
            add_flow_log_combine(pfls,ctemp);
            break;
        case FL_DOWNLOAD_BEGIN:
            sprintf(ctemp,"\"download_begin\":%lld",data);
            sprintf(pfls->flow_log_info,"%s,%s",pfls->flow_log_info,ctemp);
            break;
        case FL_DNS_BEGIN:
            sprintf(ctemp,"\"dns_begin\":%lld",data);
            add_flow_log_combine(pfls,ctemp);
            break;
        case FL_DNS_FINISH:
            sprintf(ctemp,"\"dns_finish\":%lld",data);
            add_flow_log_combine(pfls,ctemp);
            break;
        case FL_TCP_CONNECT_BEGIN:
            sprintf(ctemp,"\"tcp_connect_begin\":%lld",data);
            add_flow_log_combine(pfls,ctemp);
            break;
        case FL_TCP_CONNECT_FINISH:
            sprintf(ctemp,"\"tcp_connect_finish\":%lld",data);
            add_flow_log_combine(pfls,ctemp);
            break;
        case FL_HTTP_RESPONSE:
            sprintf(ctemp,"\"http_response\":%lld",data);
            add_flow_log_combine(pfls,ctemp);
            break;
        case FL_DOWNLOAD_FINISH:
            sprintf(ctemp,"\"download_finish\":%lld",data);
            add_flow_log_combine(pfls,ctemp);
            if (1 == pfls->flow_log_need_send)
            {
                //av_log(NULL, AV_LOG_INFO,"PLAY_FLOW_LOG:%s\n",pfls->flow_log_info);
                pfls->flow_log_need_send=0;
                ffPlayer_play_log(app_ctx,FFPLAYER_TIME_LOG_FLOW_LOG,LOG_TAG_PLAY_FLOW_LOG,"PLAY_FLOW_LOG=%s",pfls->flow_log_info);
            }
            break;
        case FL_HTTP_RESPONSE_CODE:
            sprintf(ctemp,"\"http_response_code\":%lld",data);
            add_flow_log_combine(pfls,ctemp);
            break;
        case FL_DOWNLOAD_ERROR_CODE:
            sprintf(ctemp,"\"error_code\":%lld",data);
            add_flow_log_combine(pfls,ctemp);
            break;
        case FL_TCP_READ_ERROR:
            sprintf(ctemp,"\"tcp_read_error\":%lld",data);
            add_flow_log_combine(pfls,ctemp);
            break;
        case FL_READ_SIZE:
            sprintf(ctemp,"\"read_size\":%lld",data);
            add_flow_log_combine(pfls,ctemp);
            break;
        case FL_FILE_TYPE:
            sprintf(ctemp,"\"file_type\":%s",data);
            add_flow_log_combine(pfls,ctemp);
            break;
        default:
            break;
    }
}


void add_tcp_rwtimeout_log_begin(void *app_ctx, void *uss, const char *key, const char *value)
{
    AVApplicationContext *ctx = (AVApplicationContext *)app_ctx;
    URLStartStatus* puss = (URLStartStatus*)uss;
	
    if (!ctx || !puss)
        return;
	
	FlowLogStatus *pfls = puss->fls;
    
    memset(pfls->tcp_rwtimeout_log,0,sizeof(pfls->tcp_rwtimeout_log));
    char ctemp[512]={0};
    if(value!=NULL){
        char* pfile = strrchr(value, '/');
        if (pfile!=NULL) {
            snprintf(ctemp, sizeof(ctemp), "\"%s\":\"%s\"",key, pfile+1 );
        }
    }
    else{
        snprintf(ctemp, sizeof(ctemp), "\"%s\":\"%s\"",key, "null" );
    }
    snprintf(pfls->tcp_rwtimeout_log, sizeof(pfls->tcp_rwtimeout_log), "%s",ctemp);
}

void add_tcp_rwtimeout_log_end(void *app_ctx, void *uss, const char* key, int64_t value)
{
    AVApplicationContext *ctx = (AVApplicationContext *)app_ctx;
    URLStartStatus* puss = (URLStartStatus*)uss;
	
    if (!ctx || !puss)
        return;
	
	FlowLogStatus *pfls = puss->fls;
    
    char ctemp[512]={0};
    char result[512]={0};
    snprintf(ctemp, sizeof(ctemp), "\"%s\":%lld",key, value);
    snprintf(result, sizeof(result), "%s,%s", pfls->tcp_rwtimeout_log, ctemp);
    ffPlayer_play_log(ctx, FFPLAYER_TIME_LOG_TCP, LOG_TAG_TCP_RWTIMEOUT, "%s", result);
}

void init_tcp_connection_logs(void *app_ctx, void *uss){
    AVApplicationContext *ctx = (AVApplicationContext *)app_ctx;
    URLStartStatus* puss = (URLStartStatus*)uss;
	
    if (!ctx || !puss)
        return;
	
	FlowLogStatus *pfls = puss->fls;
	
    memset(pfls->tcp_connection_logs,0,sizeof(pfls->tcp_connection_logs));
}

const char* get_tcp_connection_logs(void *app_ctx, void *uss){
    AVApplicationContext *ctx = (AVApplicationContext *)app_ctx;
    URLStartStatus* puss = (URLStartStatus*)uss;
	
    if (!ctx || !puss)
        return NULL;
	
	FlowLogStatus *pfls = puss->fls;
	
    return pfls->tcp_connection_logs;
}

void add_tcp_connection_log(void *app_ctx, void *uss, int64_t tcp_connect_begin, int64_t tcp_connect_finish, const char *remote_ip, int error_code)
{
    AVApplicationContext *ctx = (AVApplicationContext *)app_ctx;
    URLStartStatus* puss = (URLStartStatus*)uss;
	
    if (!ctx || !puss)
        return;
	
	FlowLogStatus *pfls = puss->fls;
	
	
    char connect_info[TCP_CONNECTION_LOG_SIZE]={0};
    snprintf(connect_info, sizeof(connect_info), "{\"%s\":%lld,\"%s\":%lld,\"%s\":\"%s\",\"%s\":%d}",
            "tcp_connect_begin", tcp_connect_begin,
            "tcp_connect_finish", tcp_connect_finish,
            "remote_ip", strlen(remote_ip)>0?remote_ip:"null",
            "error_code", error_code);
    
    int totallen = strlen(pfls->tcp_connection_logs);
    int currentlen = strlen(connect_info);
    if( totallen>0 ){
        if (totallen + currentlen +3 < TCP_CONNECTION_LOG_TOTAL_SIZE ) {
            snprintf(pfls->tcp_connection_logs, sizeof(pfls->tcp_connection_logs), "%s,%s", pfls->tcp_connection_logs, connect_info);
        }
        else {
            av_log(NULL, AV_LOG_WARNING, "tcp connection logs size out of range, max_size=%d, target_size=%d", TCP_CONNECTION_LOG_TOTAL_SIZE, totallen + currentlen );
        }
    }
    else
        strcpy(pfls->tcp_connection_logs, connect_info);
}
