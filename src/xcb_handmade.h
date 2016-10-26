/*
 * Copyright (c) 2014, Neil Blakey-Milner
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#define HHXCB_STATE_FILE_NAME_LENGTH (1024)
#define HHXCB_CLOCK CLOCK_MONOTONIC
#define HHXCB_MAX_CONTROLLERS 4

struct hhxcb_memory_block
{
    hhxcb_memory_block *Prev;
    hhxcb_memory_block *Next;
    u64 Size;
    u64 Pad[5];
};

inline void *GetBasePointer(hhxcb_memory_block *Block)
{
	void *Result = Block + 1;
	return(Result);
}

struct hhxcb_saved_memory_block
{
	u64 BasePointer;
	u64 Size;
};

struct hhxcb_state
{
    // NOTE(casey): To touch the memory ring, you must
    // take the memory mutex!
    ticket_mutex MemoryMutex;
    hhxcb_memory_block MemorySentinel;
    
    uint32_t recording_fd;
    uint32_t recording_index;

    uint32_t playback_fd;
    uint32_t playback_index;

    char binary_name[HHXCB_STATE_FILE_NAME_LENGTH];
    char *one_past_binary_filename_slash;
};

struct platform_work_queue_entry
{
	platform_work_queue_callback* Callback;
	void* Data;
};

struct platform_work_queue
{
	uint32 volatile CompletionGoal;
	uint32 volatile CompletionCount;
	uint32 volatile NextEntryToWrite;
	uint32 volatile NextEntryToRead;

	sem_t* SemaphoreHandle;
	
	platform_work_queue_entry Entries[256];
};

struct hhxcb_thread_startup
{
    platform_work_queue *Queue;
};

struct hhxcb_game_code
{
    void *library_handle;
    game_update_and_render *UpdateAndRender;
    game_get_sound_samples *GetSoundSamples;

	debug_game_frame_end *DEBUGFrameEnd;
	
    bool32 is_valid;
    time_t mainExecutableMtime;
    time_t library_mtime;
};

struct hhxcb_offscreen_buffer
{
    xcb_image_t *xcb_image;
    xcb_pixmap_t xcb_pixmap_id;
    xcb_gcontext_t xcb_gcontext_id;

    void *memory;
    u32 width;
    u32 height;
    s32 pitch;
    s32 bytes_per_pixel;
};

struct hhxcb_window_dimension
{
	Window rootWindow;
	s32 x;
	s32 y;
	u32 width;
	u32 height;
	u32 borderWidth;
	u32 depth;
};

struct hhxcb_controller_info {
    bool32 is_active;
    char path[HHXCB_STATE_FILE_NAME_LENGTH]; // /dev/input filename of the controller
    int fd;
    int button_ioctl_to_use;
    uint16 axis_dead_zones[ABS_MAX+1];
    bool axis_inverted[ABS_MAX+1];
    int left_thumb_x_axis;
    int left_thumb_y_axis;
    int right_thumb_x_axis;
    int right_thumb_y_axis;
    int dpad_x_axis;
    int dpad_y_axis;
    int a_button;
    int b_button;
    int x_button;
    int y_button;
    int l1_button;
    int r1_button;
    int back_button;
    int start_button;
};

enum modifiers
{
	MOD_SHIFT,
	MOD_ALT,
	MOD_CONTROL,
};

typedef u32 glx_swap_interval_mesa(s32 interval);
typedef GLXContext glx_create_context_attribs_arb(Display *display, GLXFBConfig config, GLXContext share_context, Bool direct, const int *attrib_list);

struct hhxcb_context
{
    bool32 ending_flag;

    const xcb_setup_t *setup;
    xcb_format_t *fmt;
    xcb_connection_t *connection;
    xcb_key_symbols_t *key_symbols;
    xcb_window_t window;
	Display *display;
    GLXFBConfig FBConfig;

    b32 useSoftwareRendering;
    
	GLuint openGLTextureHandle;
    glx_swap_interval_mesa *glXSwapInterval;
    glx_create_context_attribs_arb *glXCreateContextAttribsARB;

    xcb_atom_t wm_protocols;
    xcb_atom_t wm_delete_window;

    xcb_pixmap_t cursor_pixmap_id;
    xcb_cursor_t cursor_id;

    bool32 need_controller_refresh;
    hhxcb_controller_info controller_info[HHXCB_MAX_CONTROLLERS];

	game_button_state modifier_keys[3];
	
    snd_pcm_t *alsa_handle;
    snd_output_t *alsa_log;
};

struct hhxcb_sound_output
{
    int samples_per_second;
    uint32 running_sample_index;
    int bytes_per_sample;
    u32 buffer_size_in_samples;
	u32 buffer_size_in_bytes;
	uint32 safety_samples;
	u32 channels;
	u64 period_size;
};
