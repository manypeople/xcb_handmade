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

/*
  TODO: implemented in casey's platform layer, but not this one yet

   - fullscreen toggle
   - desktop fade in/out
   - replace xcb with xlib
   - get window resize working properly
*/

#include <sys/mman.h> // mmap, PROT_*, MAP_*
#include <sys/stat.h> // stat, fstat

#include <dirent.h>   // DIR *, opendir
#include <dlfcn.h>    // dlopen, dlsym, dlclose
#include <errno.h>    // errno
#include <fcntl.h>    // open, O_RDONLY
#include <stdio.h>    // printf
#include <stdlib.h>   // malloc, calloc, free
#include <string.h>   // strlen (I'm so lazy)
#include <time.h>     // clock_gettime, CLOCK_MONOTONIC
#include <unistd.h>   // readlink
#include <pthread.h>  // pthread_create, pthread_attr_init, pthread_self
#include <semaphore.h>// sem_init, sem_post, sem_wait, sem_destroy
// Note: this gives a compiler error for "_Atomic"
//#include <stdatomic.h>// atomic_fetch_add, etc...
#include <dirent.h>   // opendir, readdir, closedir
#include <wait.h>     // waitpid

#include <xcb/xcb.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xlib-xcb.h>

/*
 * Some versions of this header use class as a function argument name, which
 * C++ does not like...
 */
#define class class_name
#include <xcb/xcb_icccm.h>
#undef class

#include <xcb/xcb_image.h>
#include <xcb/xcb_keysyms.h>

#include <xcb/randr.h> // get monitor resolution for fullscreen

#include <X11/keysym.h>
#include <X11/XF86keysym.h>

#include <linux/joystick.h>

#include <alsa/asoundlib.h>

#include <GL/glx.h>

// NOTE: casey searches for these strings, so if they are defined,
// the search string will be replaced by the preprocessor
#undef GL_EXT_texture_sRGB
#undef GL_EXT_framebuffer_sRGB

#include "handmade_platform.h"
#include "xcb_handmade.h"
#include "handmade_shared.h"

platform_api Platform;

global_variable b32 OpenGLSupportsSRGBFramebuffer;
global_variable GLuint OpenGLDefaultInternalTextureFormat;
global_variable GLuint OpenGLReservedBlitTexture;

global_variable hhxcb_state GlobalHhxcbState;
global_variable b32 GlobalSoftwareRendering;
global_variable u32 GlobalWindowWidth;
global_variable u32 GlobalWindowHeight;
global_variable b32 GlobalPause;
global_variable b32 GlobalShowSortGroups;

#undef GL_ARB_framebuffer_object

typedef void gl_bind_framebuffer(GLenum target, GLuint framebuffer);
typedef void gl_gen_framebuffers(GLsizei n, GLuint *framebuffers);
typedef void gl_framebuffer_texture_2D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level);
typedef u32 gl_check_framebuffer_status(GLenum target);

global_variable gl_bind_framebuffer *glBindFramebuffer;
global_variable gl_gen_framebuffers *glGenFramebuffers;
global_variable gl_framebuffer_texture_2D *glFramebufferTexture2D;
global_variable gl_check_framebuffer_status *glCheckFramebufferStatus;

#include "handmade_sort.cpp"
#include "handmade_render.h"
#include "handmade_opengl.cpp"
#include "handmade_render.cpp"

#define internal static
#define local_persist static
#define global_variable static
#if HANDMADE_SLOW
#define Assert(Expression) if(!(Expression)) {*(int *)0 = 0;}
#else
#define Assert(Expression)
#endif
#define ArrayCount(Array) (sizeof(Array) / sizeof((Array)[0]))


internal real32
hhxcb_process_controller_axis(int16 value, uint16 dead_zone, bool inverted)
{
    if (inverted)
    {
        value = -value;
    }
    if (abs(value) < dead_zone)
    {
        return 0;
    }
    else if (value < -dead_zone)
    {
        return (real32)((value + dead_zone) / (32767.0f - dead_zone));
    }
    return (real32)((value - dead_zone) / (32767.0f - dead_zone));
}

internal void
hhxcb_process_button(bool down, game_button_state *old_state, game_button_state *new_state)
{
    new_state->EndedDown = down;
    new_state->HalfTransitionCount = (old_state->EndedDown != new_state->EndedDown) ? 1 : 0;
}

xcb_format_t *hhxcb_find_format(hhxcb_context *context, uint32_t pad, uint32_t depth, uint32_t bpp)
{
    xcb_format_t *fmt = xcb_setup_pixmap_formats(context->setup);
    xcb_format_t *fmtend = fmt + xcb_setup_pixmap_formats_length(context->setup);
    while (fmt++ != fmtend)
    {
        if (fmt->scanline_pad == pad && fmt->depth == depth && fmt->bits_per_pixel == bpp)
        {
            return fmt;
        }
    }
    return (xcb_format_t *)0;
}

xcb_image_t *
hhxcb_create_image(uint32_t pitch, uint32_t width, uint32_t height,
				   xcb_format_t *fmt, const xcb_setup_t *setup)
{
    size_t image_size = pitch * height;
    uint8_t *image_data = (uint8_t *)calloc(1, image_size);
    //uint8_t *image_data = (uint8_t *)malloc(image_size);

     return xcb_image_create(width, height, XCB_IMAGE_FORMAT_Z_PIXMAP,
             fmt->scanline_pad, fmt->depth, fmt->bits_per_pixel, 0,
             (xcb_image_order_t)setup->image_byte_order,
             XCB_IMAGE_ORDER_LSB_FIRST, image_data, image_size, image_data);
}

internal void
hhxcb_resize_backbuffer(hhxcb_context *context, hhxcb_offscreen_buffer *buffer, int width, int height)
{
    // xcb_image owns the referenced data, so will clean it up without an explicit free()
    if (buffer->xcb_image)
    {
        xcb_image_destroy(buffer->xcb_image);
    }

    if (buffer->xcb_pixmap_id)
    {
        xcb_free_pixmap(context->connection, buffer->xcb_pixmap_id);
    }

    if (buffer->xcb_gcontext_id)
    {
        xcb_free_gc(context->connection, buffer->xcb_gcontext_id);
    }

    buffer->width = width;
    buffer->height = height;
    buffer->bytes_per_pixel = context->fmt->bits_per_pixel / 8;
	// NOTE: round the pitch up to a sixteen byte boundary
    buffer->pitch = Align16(buffer->bytes_per_pixel * width);

    buffer->xcb_image = hhxcb_create_image(buffer->pitch, width, height,
										   context->fmt, context->setup);
    buffer->memory = buffer->xcb_image->data;

    buffer->xcb_pixmap_id = xcb_generate_id(context->connection);
    xcb_create_pixmap(context->connection, context->fmt->depth,
            buffer->xcb_pixmap_id, context->window, width, height);

    buffer->xcb_gcontext_id = xcb_generate_id (context->connection);
    xcb_create_gc (context->connection, buffer->xcb_gcontext_id,
            buffer->xcb_pixmap_id, 0, 0);
}

internal void
load_and_set_cursor(hhxcb_context *context)
{
    context->cursor_pixmap_id = xcb_generate_id(context->connection);
    xcb_create_pixmap(context->connection, /*depth*/ 1,
            context->cursor_pixmap_id, context->window, /*width*/ 1,
            /*height*/ 1);

    context->cursor_id = xcb_generate_id(context->connection);
    xcb_create_cursor(context->connection, context->cursor_id,
            context->cursor_pixmap_id, context->cursor_pixmap_id, 0, 0, 0, 0,
            0, 0, 0, 0);

    uint32_t values[1] = { context->cursor_id };
    xcb_change_window_attributes( context->connection, context->window,
            XCB_CW_CURSOR, values);
}

internal void
load_atoms(hhxcb_context *context)
{
    xcb_intern_atom_cookie_t wm_delete_window_cookie =
        xcb_intern_atom(context->connection, 0, 16, "WM_DELETE_WINDOW");
    xcb_intern_atom_cookie_t wm_protocols_cookie =
        xcb_intern_atom(context->connection, 0, strlen("WM_PROTOCOLS"),
                "WM_PROTOCOLS");

    xcb_flush(context->connection);
    xcb_intern_atom_reply_t* wm_delete_window_cookie_reply =
        xcb_intern_atom_reply(context->connection, wm_delete_window_cookie, 0);
    xcb_intern_atom_reply_t* wm_protocols_cookie_reply =
        xcb_intern_atom_reply(context->connection, wm_protocols_cookie, 0);

    context->wm_protocols = wm_protocols_cookie_reply->atom;
    context->wm_delete_window = wm_delete_window_cookie_reply->atom;
}

#if HANDMADE_INTERNAL
DEBUG_PLATFORM_FREE_FILE_MEMORY(debug_xcb_free_file_memory)
{
    free(Memory);
}

DEBUG_PLATFORM_READ_ENTIRE_FILE(debug_xcb_read_entire_file)
{
    debug_read_file_result result = {};

    FILE *f = fopen(Filename, "rb");

    if (f == 0)
    {
        printf("Failed to open file %s\n", Filename);
        return result;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *string = (char *)malloc(fsize + 1);
    fread(string, fsize, 1, f);
    fclose(f);

    string[fsize] = 0;

    result.Contents = string;
    result.ContentsSize = fsize;

    return(result);
}

DEBUG_PLATFORM_WRITE_ENTIRE_FILE(debug_xcb_write_entire_file)
{
    FILE *f = fopen(Filename, "wb");

    if (f == 0)
    {
        printf("Failed to open file %s\n", Filename);
        return 0;
    }

    size_t res = fwrite(Memory, 1, MemorySize, f);
    fclose(f);

    return res == MemorySize;
}

internal b32
hhxcbSearchString(char *StringToBeSearched, char *StringToSearchFor)
{
	u32 MatchIndex = 0;
	for(u32 charAt = 0;
		StringToBeSearched[charAt];
		++charAt)
	{
		if(StringToBeSearched[charAt] == StringToSearchFor[MatchIndex])
		{
			++MatchIndex;
			if(StringToSearchFor[MatchIndex] == 0)
			{
				// NOTE: string found
				return true;
			}
		}
		else
		{
			MatchIndex = 0;
		}
	}
	// NOTE: string not found
	return false;
}

DEBUG_PLATFORM_EXECUTE_SYSTEM_COMMAND(debug_execute_system_command)
{
    debug_executing_process Result = {};

	if(hhxcbSearchString(Command, "cmd.exe"))
	{
		Command = "/usr/bin/sh";
	}
	if(hhxcbSearchString(CommandLine, "build.bat"))
	{
		CommandLine = "../../build.sh";
	}
	
	pid_t forkPID = vfork(); //fork();

	if(forkPID > 0)
    {
        Assert(sizeof(Result.OSHandle) >= sizeof(forkPID));
		Result.OSHandle = forkPID;
    }
    else if(forkPID == 0)
    {
		execl(Command, Command, CommandLine, (char *)0);
		
		// NOTE: make sure this point isn't reached
		Assert(0);
    }

    return(Result);
}

DEBUG_PLATFORM_GET_PROCESS_STATE(debug_get_process_state)
{
    debug_process_state Result = {};

    if(Process.OSHandle != 0)
    {
        Result.StartedSuccessfully = true;
		
		s32 status = 0;
		if(waitpid(Process.OSHandle, &status, WNOHANG) == 0)
		{
            Result.IsRunning = true;
		}
		else if(WIFEXITED(status))
        {
            Result.ReturnCode = WEXITSTATUS(status);
		}
    }

    return(Result);
}

#endif

inline timespec
hhxcbGetWallClock(void)
{   
	timespec result = {};
	// Actually monotonic clock
	clock_gettime(HHXCB_CLOCK, &result);
	return result;
}

inline real32
hhxcbGetSecondsElapsed(timespec start, timespec end)
{
	u32 wholeSeconds = end.tv_sec - start.tv_sec;
	r32 partialSeconds = (end.tv_nsec - start.tv_nsec) / 1000000000.0f;
	
	r32 result = (r32)wholeSeconds + partialSeconds;
	return result;
}

internal void
hhxcb_get_binary_name(hhxcb_state *state)
{
    // NOTE(nbm): There are probably pathological cases where this won't work - for
    // example if the underlying file has been removed/moved.
    readlink("/proc/self/exe", state->binary_name, HHXCB_STATE_FILE_NAME_LENGTH);
    for (char *c = state->binary_name; *c; ++c)
    {
        if (*c == '/')
        {
            state->one_past_binary_filename_slash = c + 1;
        }
    }
}

internal void
hhxcb_cat_strings(
    size_t src_a_count, char *src_a,
    size_t src_b_count, char *src_b,
    size_t dest_count, char *dest
    )
{
    size_t counter = 0;
    for (
            size_t i = 0;
            i < src_a_count && counter++ < dest_count;
            ++i)
    {
        *dest++ = *src_a++;
    }
    for (
            size_t i = 0;
            i < src_b_count && counter++ < dest_count;
            ++i)
    {
        *dest++ = *src_b++;
    }

    *dest++ = 0;
}

internal void
hhxcb_build_full_filename(hhxcb_state *state, char *filename, int dest_count, char *dest)
{
    hhxcb_cat_strings(state->one_past_binary_filename_slash -
            state->binary_name, state->binary_name, strlen(filename), filename,
            dest_count, dest);
}

internal void
hhxcb_load_game(hhxcb_game_code *game_code, char *path)
{
    struct stat statbuf = {};
    uint32_t stat_result = stat(path, &statbuf);
    if (stat_result != 0)
    {
        printf("Failed to stat game code at %s", path);
        return;
    }
    game_code->library_mtime = statbuf.st_mtime;

    game_code->is_valid = false;
    game_code->library_handle = dlopen(path, RTLD_LAZY);
    if (game_code->library_handle == 0)
    {
        char *error = dlerror();
        printf("Unable to load library at path %s: %s\n", path, error);
        return;
    }
    game_code->UpdateAndRender =
        (game_update_and_render *)dlsym(game_code->library_handle, "GameUpdateAndRender");
    if (game_code->UpdateAndRender == 0)
    {
        char *error = dlerror();
        printf("Unable to load symbol GameUpdateAndRender: %s\n", error);
        free(error);
        return;
    }
    game_code->GetSoundSamples =
        (game_get_sound_samples *)dlsym(game_code->library_handle, "GameGetSoundSamples");
    if (game_code->GetSoundSamples == 0)
    {
        char *error = dlerror();
        printf("Unable to load symbol GameGetSoundSamples: %s\n", error);
        free(error);
        return;
    }
	game_code->DEBUGFrameEnd =
        (debug_game_frame_end *)dlsym(game_code->library_handle, "DEBUGGameFrameEnd");
    if (game_code->DEBUGFrameEnd == 0)
    {
        char *error = dlerror();
        printf("Unable to load symbol DEBUGGameFrameEnd: %s\n", error);
        free(error);
        return;
    }

    game_code->is_valid = game_code->library_handle &&
        game_code->UpdateAndRender && game_code->GetSoundSamples &&
        game_code->DEBUGFrameEnd;

    if(!game_code->is_valid)
    {
        game_code->library_handle = 0;
        game_code->UpdateAndRender = 0;
        game_code->GetSoundSamples = 0;
        game_code->DEBUGFrameEnd = 0;        
    }
}

internal void
hhxcb_unload_game(hhxcb_game_code *game_code)
{
    if (game_code->library_handle)
    {
        dlclose(game_code->library_handle);
        game_code->library_handle = 0;
    }
    game_code->is_valid = 0;
    game_code->UpdateAndRender = 0;
    game_code->GetSoundSamples = 0;
}

internal void
hhxcb_get_input_file_location(hhxcb_state *state, bool32 input_stream, uint index, int dest_size, char *dest)
{
    char temp[64];
    sprintf(temp, "loop_edit_%d_%s.hmi", index, input_stream ? "input" : "state");
    hhxcb_build_full_filename(state,
            temp,
            dest_size,
            dest);
}

internal
DEBUG_PLATFORM_GET_MEMORY_STATS(hhxcbGetMemoryStats)
{
    debug_platform_memory_stats Stats = {};
    
    BeginTicketMutex(&GlobalHhxcbState.MemoryMutex);
    hhxcb_memory_block *Sentinel = &GlobalHhxcbState.MemorySentinel;
    for(hhxcb_memory_block *SourceBlock = Sentinel->Next;
        SourceBlock != Sentinel;
        SourceBlock = SourceBlock->Next)
    {
        Assert(SourceBlock->Block.Size <= U32Maximum);
        
        ++Stats.BlockCount;
        Stats.TotalSize += SourceBlock->Block.Size;
        Stats.TotalUsed += SourceBlock->Block.Used;
    }
    EndTicketMutex(&GlobalHhxcbState.MemoryMutex);
    
    return(Stats);
}

internal void
hhxcbVerifyMemoryListIntegrity(void)
{
    BeginTicketMutex(&GlobalHhxcbState.MemoryMutex);
    local_persist u32 FailCounter;
    hhxcb_memory_block *Sentinel = &GlobalHhxcbState.MemorySentinel;
    for(hhxcb_memory_block *SourceBlock = Sentinel->Next;
        SourceBlock != Sentinel;
        SourceBlock = SourceBlock->Next)
    {
        Assert(SourceBlock->Block.Size <= U32Maximum);
    }
    ++FailCounter;
    if(FailCounter == 35)
    {
        int BreakHere = 3;
    }
    EndTicketMutex(&GlobalHhxcbState.MemoryMutex);
}

internal void
hhxcb_start_recording(hhxcb_state *state, uint8 index)
{
    char filename[HHXCB_STATE_FILE_NAME_LENGTH];
    hhxcb_get_input_file_location(state, true, index, sizeof(filename), filename);
    state->recording_fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);

    if(state->recording_fd != -1)
    {
        state->recording_index = index;
    
        hhxcb_memory_block *Sentinel = &GlobalHhxcbState.MemorySentinel;

        BeginTicketMutex(&GlobalHhxcbState.MemoryMutex);
        for(hhxcb_memory_block *SourceBlock = Sentinel->Next;
            SourceBlock != Sentinel;
            SourceBlock = SourceBlock->Next)
        {
            if(!(SourceBlock->Block.Flags & PlatformMemory_NotRestored))
            {
                hhxcb_saved_memory_block DestBlock;
                void *BasePointer = SourceBlock->Block.Base;
                DestBlock.BasePointer = (u64)BasePointer;
                DestBlock.Size = SourceBlock->Block.Size;
                write(state->recording_fd, &DestBlock, sizeof(DestBlock));
                Assert(DestBlock.Size <= U32Maximum);
                write(state->recording_fd, BasePointer, (u32)DestBlock.Size);
            }
        }
        EndTicketMutex(&GlobalHhxcbState.MemoryMutex);

        hhxcb_saved_memory_block DestBlock = {};
        write(state->recording_fd, &DestBlock, sizeof(DestBlock));
    }
}

internal void
hhxcb_stop_recording(hhxcb_state *state)
{
    close(state->recording_fd);
    state->recording_index = 0;
    state->recording_fd = 0;
}

internal void
hhxcbFreeMemoryBlock(hhxcb_memory_block *Block)
{
    BeginTicketMutex(&GlobalHhxcbState.MemoryMutex);
    Block->Prev->Next = Block->Next;
    Block->Next->Prev = Block->Prev;
    EndTicketMutex(&GlobalHhxcbState.MemoryMutex);
		
    smm Result = munmap(Block, Block->Block.Size + Block->AdditionalSize);
    Assert(Result != -1);
}

internal void
hhxcbClearBlocksByMask(hhxcb_state *State, u64 Mask)
{
    for(hhxcb_memory_block *BlockIter = State->MemorySentinel.Next;
        BlockIter != &State->MemorySentinel;
        )
    {
        hhxcb_memory_block *Block = BlockIter;
        BlockIter = BlockIter->Next;
        
        if((Block->LoopingFlags & Mask) == Mask)
        {
            hhxcbFreeMemoryBlock(Block);
        }
        else
        {
            Block->LoopingFlags = 0;
        }
    }
}

internal void
hhxcb_start_playback(hhxcb_state *state, uint8 index)
{
    hhxcbClearBlocksByMask(state, hhxcbMem_AllocatedDuringLooping);

    char filename[HHXCB_STATE_FILE_NAME_LENGTH];
    hhxcb_get_input_file_location(state, true, index, sizeof(filename), filename);
    state->playback_fd = open(filename, O_RDONLY);

    if(state->playback_fd != -1)
    {
        state->playback_index = index;

        for(;;)
        {
            hhxcb_saved_memory_block Block = {};

            read(state->playback_fd, &Block, sizeof(Block));
            if(Block.BasePointer != 0)
            {
                void *BasePointer = (void *)Block.BasePointer;
                Assert(Block.Size <= U32Maximum);
                read(state->playback_fd, BasePointer, (u32)Block.Size);
            }
            else
            {
                break;
            }
        }
        // TODO(casey): Stream memory in from the file!
    }
}

internal void
hhxcb_stop_playback(hhxcb_state *state)
{
    hhxcbClearBlocksByMask(state, hhxcbMem_FreedDuringLooping);
    close(state->playback_fd);
    state->playback_index = 0;
    state->playback_fd = 0;
}

internal void
hhxcb_record_input(hhxcb_state *state, game_input *new_input)
{
    write(state->recording_fd, new_input, sizeof(*new_input));
}

internal void
hhxcb_playback_input(hhxcb_state *state, game_input *new_input)
{
    int bytes_read = read(state->playback_fd, new_input, sizeof(*new_input));
    if (bytes_read == 0)
    {
        uint8 index = state->playback_index;
        hhxcb_stop_playback(state);
        hhxcb_start_playback(state, index);
        read(state->playback_fd, new_input, sizeof(*new_input));
    }
}

internal void
hhxcb_process_keyboard_message(game_button_state *new_state, bool32 is_down)
{
    if (new_state->EndedDown != is_down)
    {
        new_state->EndedDown = is_down;
        ++new_state->HalfTransitionCount;
    }
}

internal void
hhxcb_process_events(hhxcb_context *context, hhxcb_state *state, hhxcb_offscreen_buffer *buffer, game_render_commands *RenderCommands, rectangle2i *DrawRegion, game_input *new_input, game_input *old_input)
{
    game_controller_input *old_keyboard_controller = GetController(old_input, 0);
    game_controller_input *new_keyboard_controller = GetController(new_input, 0);

    *new_keyboard_controller = {};
    new_keyboard_controller->IsConnected = true;
    for (
        uint button_index = 0;
        button_index < ArrayCount(new_keyboard_controller->Buttons);
        ++button_index)
    {
        new_keyboard_controller->Buttons[button_index].EndedDown =
            old_keyboard_controller->Buttons[button_index].EndedDown;
    }

    new_input->MouseX = old_input->MouseX;
    new_input->MouseY = old_input->MouseY;
    new_input->MouseZ = old_input->MouseZ;

    new_input->ShiftDown = context->modifier_keys[MOD_SHIFT].EndedDown;
    new_input->AltDown = context->modifier_keys[MOD_ALT].EndedDown;
    new_input->ControlDown = context->modifier_keys[MOD_CONTROL].EndedDown;
	
    for(u32 ButtonIndex = 0;
        ButtonIndex < PlatformMouseButton_Count;
        ++ButtonIndex)
    {
        new_input->MouseButtons[ButtonIndex] = old_input->MouseButtons[ButtonIndex];
        new_input->MouseButtons[ButtonIndex].HalfTransitionCount = 0;
    }

    {
        TIMED_BLOCK("XBox Controllers");

        for (int i = 0; i < HHXCB_MAX_CONTROLLERS; ++i)
        {
            hhxcb_controller_info *pad = &context->controller_info[i];
            if (!pad->is_active)
            {
                continue;
            }
            game_controller_input *old_controller = GetController(old_input, i+1);
            game_controller_input *new_controller = GetController(new_input, i+1);

            *new_controller = *old_controller;
            new_controller->IsConnected = true;
            new_controller->IsAnalog = old_controller->IsAnalog;

            js_event j;
            while (read(pad->fd, &j, sizeof(js_event)) == sizeof(js_event))
            {
                // Don't care if init or afterwards
                j.type &= ~JS_EVENT_INIT;
                if (j.type == JS_EVENT_BUTTON)
                {
                    if (j.number == pad->a_button)
                    {
                        hhxcb_process_button((j.value > 0),
                                             &old_controller->ActionDown,
                                             &new_controller->ActionDown);
                    }
                    else if (j.number == pad->b_button)
                    {
                        hhxcb_process_button((j.value > 0),
                                             &old_controller->ActionRight,
                                             &new_controller->ActionRight);
                    }
                    else if (j.number == pad->x_button)
                    {
                        hhxcb_process_button((j.value > 0),
                                             &old_controller->ActionLeft,
                                             &new_controller->ActionLeft);
                    }
                    else if (j.number == pad->y_button)
                    {
                        hhxcb_process_button((j.value > 0),
                                             &old_controller->ActionUp,
                                             &new_controller->ActionUp);
                    }
                    else if (j.number == pad->l1_button)
                    {
                        hhxcb_process_button((j.value > 0),
                                             &old_controller->LeftShoulder,
                                             &new_controller->LeftShoulder);
                    }
                    else if (j.number == pad->r1_button)
                    {
                        hhxcb_process_button((j.value > 0),
                                             &old_controller->RightShoulder,
                                             &new_controller->RightShoulder);
                    }
                    else if (j.number == pad->back_button)
                    {
                        hhxcb_process_button((j.value > 0),
                                             &old_controller->Back,
                                             &new_controller->Back);
                    }
                    else if (j.number == pad->start_button)
                    {
                        hhxcb_process_button((j.value > 0),
                                             &old_controller->Start,
                                             &new_controller->Start);
                    }
                    else
                    {
                        printf("Unhandled button: number %d, value %d\n",
                               j.number, j.value);
                    }
                }
                if (j.type == JS_EVENT_AXIS)
                {
                    uint16 dead_zone = pad->axis_dead_zones[j.number];
                    bool axis_inverted = pad->axis_inverted[j.number];
                    if (j.number == pad->left_thumb_x_axis)
                    {
                        new_controller->StickAverageX =
                            hhxcb_process_controller_axis(j.value, dead_zone,
                                                          axis_inverted);
                        if (new_controller->StickAverageX != 0.0f)
                        {
                            new_controller->IsAnalog = true;
                        }
                    }
                    else if (j.number == pad->left_thumb_y_axis)
                    {
                        new_controller->StickAverageY =
                            hhxcb_process_controller_axis(j.value, dead_zone,
                                                          axis_inverted);
                        if (new_controller->StickAverageY != 0.0f)
                        {
                            new_controller->IsAnalog = true;
                        }
                    }
                    else if (j.number == pad->right_thumb_x_axis)
                    {
                        // TODO(nbm): Do something with this.  Here otherwise we get spammed.
                    }
                    else if (j.number == pad->right_thumb_y_axis)
                    {
                        // TODO(nbm): Do something with this.  Here otherwise we get spammed.
                    }
                    else if (j.number == pad->dpad_x_axis)
                    {
                        hhxcb_process_button((j.value > 0),
                                             &old_controller->MoveRight,
                                             &new_controller->MoveRight);
                        hhxcb_process_button((j.value < 0),
                                             &old_controller->MoveLeft,
                                             &new_controller->MoveLeft);
                        new_controller->IsAnalog = false;
                    }
                    else if (j.number == pad->dpad_y_axis)
                    {
                        hhxcb_process_button((j.value > 0),
                                             &old_controller->MoveDown,
                                             &new_controller->MoveDown);
                        hhxcb_process_button((j.value < 0),
                                             &old_controller->MoveUp,
                                             &new_controller->MoveUp);
                        new_controller->IsAnalog = false;
                    }
                    else
                    {
                        printf("Unhandled Axis: number %d, value %d\n",
                               j.number, j.value);
                    }

                }
            }
            if (errno != EAGAIN) {
                context->need_controller_refresh = true;
            }
        }
    }

    {
        TIMED_BLOCK("hhxcb Message/Keyboard/Mouse Processing");
        
        XEvent event;
        for(;;)
        {
            b32 gotMessage = 0;
            {
                TIMED_BLOCK("XPending");
                gotMessage = XPending(context->display);
            }
            if(!gotMessage)
            {
                break;
            }
            {
                TIMED_BLOCK("XNextEvent");
                XNextEvent(context->display, &event);
            }
            switch(event.type)
            {
                case KeyPress:
                case KeyRelease:
                {
                    XKeyEvent *e = (XKeyEvent *)&event;
                    bool32 is_down = (event.type == KeyPress);
                    KeySym keysym = XLookupKeysym(e, 0);
                    if (keysym == XK_w)
                    {
                        hhxcb_process_keyboard_message(&new_keyboard_controller->MoveUp, is_down);
                    }
                    else if (keysym == XK_a)
                    {
                        hhxcb_process_keyboard_message(&new_keyboard_controller->MoveLeft, is_down);
                    }
                    else if (keysym == XK_s)
                    {
                        hhxcb_process_keyboard_message(&new_keyboard_controller->MoveDown, is_down);
                    }
                    else if (keysym == XK_d)
                    {
                        hhxcb_process_keyboard_message(&new_keyboard_controller->MoveRight, is_down);
                    }
                    else if (keysym == XK_q)
                    {
                        hhxcb_process_keyboard_message(&new_keyboard_controller->LeftShoulder, is_down);
                    }
                    else if (keysym == XK_e)
                    {
                        hhxcb_process_keyboard_message(&new_keyboard_controller->RightShoulder, is_down);
                    }
                    else if (keysym == XK_Up)
                    {
                        hhxcb_process_keyboard_message(&new_keyboard_controller->ActionUp, is_down);
                    }
                    else if (keysym == XK_Left)
                    {
                        hhxcb_process_keyboard_message(&new_keyboard_controller->ActionLeft, is_down);
                    }
                    else if (keysym == XK_Down)
                    {
                        hhxcb_process_keyboard_message(&new_keyboard_controller->ActionDown, is_down);
                    }
                    else if (keysym == XK_Right)
                    {
                        hhxcb_process_keyboard_message(&new_keyboard_controller->ActionRight, is_down);
                    }
                    else if (keysym == XK_Escape)
                    {
                        hhxcb_process_keyboard_message(&new_keyboard_controller->Back, is_down);
                    }
                    else if (keysym == XK_space)
                    {
                        hhxcb_process_keyboard_message(&new_keyboard_controller->Start, is_down);
                    }
                    else if ((keysym == XK_Shift_L) || (keysym == XK_Shift_R))
                    {
                        hhxcb_process_keyboard_message(&context->modifier_keys[MOD_SHIFT], is_down);
                    }
                    else if ((keysym == XK_Alt_L) || (keysym == XK_Alt_R))
                    {
                        hhxcb_process_keyboard_message(&context->modifier_keys[MOD_ALT], is_down);
                    }
                    else if ((keysym == XK_Control_L) || (keysym == XK_Control_R))
                    {
                        hhxcb_process_keyboard_message(&context->modifier_keys[MOD_CONTROL], is_down);
                    }
                    else if(keysym == XK_p)
                    {
                        if(is_down)
                        {
                            GlobalPause = !GlobalPause;
                        }
                    }
                    else if (keysym == XK_l)
                    {
                        if (is_down)
                        {
                            if (state->playback_index == 0)
                            {
                                if (state->recording_index == 0)
                                {
                                    hhxcb_start_recording(state, 1);
                                }
                                else
                                {
                                    hhxcb_stop_recording(state);
                                    hhxcb_start_playback(state, 1);
                                }
                            }
                            else
                            {
                                hhxcb_stop_playback(state);
                            }
                        }
                    }
                    break;
                }
                case ButtonPress:
                case ButtonRelease:
                {
                    XButtonEvent *e = (XButtonEvent *)&event;
                    bool32 is_down = (event.type == ButtonPress);
                    if((e->button >= 1) && (e->button <= 5))
                    {
                        u32 mouseButtonIndex = (e->button - 1);
                        hhxcb_process_keyboard_message(&new_input->MouseButtons[mouseButtonIndex], is_down);
                    }
                }
                case NoExpose:
                {
                    // No idea what these are, but they're spamming me.
                    break;
                }
                case MotionNotify:
                {
                    XMotionEvent *e = (XMotionEvent *)&event;

                    r32 MouseX = (r32)e->x;
                    // NOTE: X11 window has 0,0 position in upper left
                    // corner, windows has 0,0 in lower left
                    r32 MouseY = (r32)(buffer->height - e->y);
                            
                    r32 MouseU = Clamp01MapToRange((r32)DrawRegion->MinX, MouseX, (r32)DrawRegion->MaxX);
                    r32 MouseV = Clamp01MapToRange((r32)DrawRegion->MinY, MouseY, (r32)DrawRegion->MaxY);
                            
                    new_input->MouseX = (r32)RenderCommands->Width*MouseU;
                    new_input->MouseY = (r32)RenderCommands->Height*MouseV;

                    break;
                }
                case ClientMessage:
                {
                    XClientMessageEvent *client_message_event = (XClientMessageEvent *)&event;

                    if (client_message_event->message_type == context->wm_protocols)
                    {
                        if (client_message_event->data.l[0] == context->wm_delete_window)
                        {
                            context->ending_flag = 1;
                            break;
                        }
                    }
                    break;
                }
                case ResizeRequest:
                {
                    XResizeRequestEvent *resize = (XResizeRequestEvent *)&event;
                    GlobalWindowWidth = resize->width;
                    GlobalWindowHeight = resize->height;
#if 0
                    hhxcbDisplayBufferInWindow(context, buffer,
                                               resize->width, resize->height);
#endif
                    break;
                }
                default:
                {
                    break;
                }
            }
        }
    }
}

internal void
hhxcb_refresh_controllers(hhxcb_context *context)
{
    // TODO(nbm): At the end of the function we should remove any controllers
    // that remain with is_active=false, closing their fds, and so forth.
    for(uint i = 0; i < ArrayCount(context->controller_info); i++)
    {
        context->controller_info[i].is_active = false;
    }

    DIR *dir = opendir("/dev/input");
    dirent entry;
    dirent *result;
    while (!readdir_r(dir, &entry, &result) && result)
    {
        if ((entry.d_name[0] == 'j') && (entry.d_name[1] == 's'))
        {
            char full_device_path[HHXCB_STATE_FILE_NAME_LENGTH];
            snprintf(full_device_path, sizeof(full_device_path), "%s/%s", "/dev/input", entry.d_name);
            bool found = false;
            for(uint i = 0; i < ArrayCount(context->controller_info); i++)
            {
                if (strcmp(context->controller_info[i].path, full_device_path) == 0)
                {
                    context->controller_info[i].is_active = true;
                    found = true;
                }
            }
            if (found) {
                continue;
            }

            int fd = open(full_device_path, O_RDONLY);
            if (fd < 0)
            {
                // TODO(nbm): log or something - permission may prevent open/read
                continue;
            }

            char name[128];
            ioctl(fd, JSIOCGNAME(128), name);
            if (!strstr(name, "Microsoft X-Box 360 pad"))
            {
                // TODO(nbm): log or something
                close(fd);
                continue;
            }

            hhxcb_controller_info *use = 0;
            for(uint i = 0; i < ArrayCount(context->controller_info); i++)
            {
                if (context->controller_info[i].fd <= 0)
                {
                    use = &context->controller_info[i];
                    break;
                }
            }
            if (!use) {
                // TODO(nbm): log
                close(fd);
                continue;
            }

            // Does this ever matter?
            int version;
            ioctl(fd, JSIOCGVERSION, &version);
            uint8 axes;
            ioctl(fd, JSIOCGAXES, &axes);
            uint8 buttons;
            ioctl(fd, JSIOCGBUTTONS, &buttons);
            printf("%s: %s, v%u, axes: %u, buttons: %u\n", entry.d_name, name, version, axes, buttons);

            snprintf(use->path, sizeof(use->path), "%s/%s", "/dev/input", entry.d_name);
            use->is_active = true;
            use->fd = fd;
            use->axis_dead_zones[0] = 7849;
            use->axis_dead_zones[1] = 7849;
            use->left_thumb_x_axis = 0;
            use->left_thumb_y_axis = 1;
            use->right_thumb_x_axis = 3;
            use->right_thumb_y_axis = 4;
            use->a_button = 0;
            use->b_button = 1;
            use->x_button = 2;
            use->y_button = 3;
            use->l1_button = 4;
            use->r1_button = 5;
            use->back_button = 6;
            use->start_button = 7;
            use->dpad_x_axis = 6;
            use->dpad_y_axis = 7;
            fcntl(fd, F_SETFL, O_NONBLOCK);
        }
    }
    closedir(dir);
}

internal
void hhxcb_init_alsa(hhxcb_context *context, hhxcb_sound_output *soundOutput)
{
    // NOTE: "hw:0,0" doesn't seem to work with alsa running on top of pulseaudio
    char *device = (char *)"default";
    // char *device = (char *)"hw:0,0";

    int err;
    snd_pcm_sframes_t frames;
    err = snd_output_stdio_attach(&context->alsa_log, stderr, 0);

    if ((err = snd_pcm_open(&context->alsa_handle, device, SND_PCM_STREAM_PLAYBACK, 0)) < 0)
    {
            printf("Playback open error: %s\n", snd_strerror(err));
            exit(EXIT_FAILURE);
    }

    snd_pcm_hw_params_t *hwparams;
    snd_pcm_hw_params_alloca(&hwparams);
	printf("\nSETTING HWPARAMS\n");
    snd_pcm_hw_params_any(context->alsa_handle, hwparams);
	
    err = snd_pcm_hw_params_set_access(context->alsa_handle, hwparams, SND_PCM_ACCESS_RW_INTERLEAVED);
	if(!err)
		printf("set access: SND_PCM_ACCESS_RW_INTERLEAVED\n");
    err = snd_pcm_hw_params_set_format(context->alsa_handle, hwparams, SND_PCM_FORMAT_S16_LE);
	if(!err)
		printf("set format: SND_PCM_FORMAT_S16_LE\n");
    err = snd_pcm_hw_params_set_channels(context->alsa_handle, hwparams, soundOutput->channels);
	if(!err)
		printf("set channels: %d\n", soundOutput->channels);
    err = snd_pcm_hw_params_set_rate(context->alsa_handle, hwparams, soundOutput->samples_per_second, 0);
	if(!err)
		printf("set rate: %d\n", soundOutput->samples_per_second);
	// NOTE: period is the interval between interrupts by the soundcard to
	// read the buffer
//	err = snd_pcm_hw_params_set_period_size(context->alsa_handle, hwparams, sound_output->samples_per_second / 60, 0);
//	if(!err)
//	    printf("set period size: %d\n", sound_output->samples_per_second / 60);
	
	// NOTE: "snd_pcm_hw_params_set_buffer_size" takes a buffer size
	// parameter in frames, what alsa calls a multi channel sample)
    err = snd_pcm_hw_params_set_buffer_size(context->alsa_handle, hwparams, soundOutput->buffer_size_in_samples);
	if(!err)
		printf("set buffer size: %d\n", soundOutput->buffer_size_in_samples);
	
    err = snd_pcm_hw_params(context->alsa_handle, hwparams);
	if(!err)
		printf("SET HWPARAMS and now in SND_PCM_STATE_PREPARED\n\n");

	snd_pcm_hw_params_get_period_size(hwparams, &soundOutput->period_size, 0);
	
	// NOTE: The interrupt update frequency is given in this
	// log dump, as the period size/time, on this machine it was ~241
	// samples, or ~5 milliseconds.
    snd_pcm_dump(context->alsa_handle, context->alsa_log);
}

int hhxcbOpenGLAttribs[] =
{
    GLX_CONTEXT_MAJOR_VERSION_ARB, 3,
    GLX_CONTEXT_MINOR_VERSION_ARB, 0,
    GLX_CONTEXT_FLAGS_ARB, 0 // NOTE(casey): Enable for testing WGL_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB
#if HANDMADE_INTERNAL
    |GLX_CONTEXT_DEBUG_BIT_ARB
#endif
    ,
    GLX_CONTEXT_PROFILE_MASK_ARB, GLX_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB,
    0,
};

internal GLXContext
hhxcbInitOpenGL(hhxcb_context *context)
{
    // NOTE: don't know of wglGetExtensionsString linux equivalent, so
    // just setting this to true
    OpenGLSupportsSRGBFramebuffer = true;
    
	s32 desiredVisualPixelFormat[] =
    {	
		GLX_RGBA,
		GLX_RED_SIZE, 8,
		GLX_GREEN_SIZE, 8,
		GLX_BLUE_SIZE, 8,
		GLX_ALPHA_SIZE, 8,
		GLX_DEPTH_SIZE, 24,
		GLX_STENCIL_SIZE, 8,
		GLX_DOUBLEBUFFER,
        GLX_FRAMEBUFFER_SRGB_CAPABLE_ARB, GL_TRUE,
		0
	};

	s32 screen = DefaultScreen(context->display);
	XVisualInfo *ChosenVisualInfo = glXChooseVisual(context->display,
                              screen,
                              desiredVisualPixelFormat);
	GLXContext OpenGLContext = glXCreateContext(context->display,
												ChosenVisualInfo,
												0, true);
    XFree(ChosenVisualInfo);

	if(glXMakeCurrent(context->display, context->window, OpenGLContext))
	{
        b32 ModernContext = false;

        // NOTE: I don't think this "context escalation" is necessary on
        // *nix, I think you can use glXCreateContextAttribsARB without
        // first creating a legacy context (tested, and yes you can)
        // "context escalation" is also not necessary to enable srgb
        context->glXCreateContextAttribsARB =
            (glx_create_context_attribs_arb *)glXGetProcAddressARB(
                (GLubyte *)"glXCreateContextAttribsARB");
        if(context->glXCreateContextAttribsARB)
        {
            // NOTE(casey): This is a modern version of OpenGL
            s32 desiredFBConfigPixelFormat[] = 
            {
                GLX_RENDER_TYPE, GLX_RGBA_BIT,
                GLX_RED_SIZE, 8,
                GLX_GREEN_SIZE, 8,
                GLX_BLUE_SIZE, 8,
                GLX_ALPHA_SIZE, 8,
                GLX_DEPTH_SIZE, 24,
                GLX_STENCIL_SIZE, 8,
                GLX_DOUBLEBUFFER, true,
                GLX_FRAMEBUFFER_SRGB_CAPABLE_ARB, GL_TRUE,
                0
            };

            s32 numberOfConfigsReturned = 0;
            GLXFBConfig *FBConfig = glXChooseFBConfig(context->display,
                            DefaultScreen(context->display),
                            desiredFBConfigPixelFormat,
                            &numberOfConfigsReturned);
            
            // NOTE: just use the first FBConfig in the list returned
            if(numberOfConfigsReturned > 0)
            {
                context->FBConfig = FBConfig[0];
                GLXContext shareContext = 0;
                GLXContext ModernOpenGLContext = 
                    context->glXCreateContextAttribsARB(context->display, context->FBConfig, shareContext, true, hhxcbOpenGLAttribs);
                if(ModernOpenGLContext)
                {
                    if(glXMakeCurrent(context->display, context->window,
                                      ModernOpenGLContext))
                    {
                        ModernContext = true;
                        glXDestroyContext(context->display, OpenGLContext);
                        OpenGLContext = ModernOpenGLContext;
                    }
                }
            }
        }
        else
        {
            // NOTE(casey): This is an antiquated version of OpenGL
        }

        // NOTE: this function also turns srgb on if it is supported
        opengl_info Info = OpenGLInit(ModernContext, OpenGLSupportsSRGBFramebuffer);
        if(Info.GL_ARB_framebuffer_object)
        {
            glBindFramebuffer = (gl_bind_framebuffer *)glXGetProcAddress((const GLubyte*)"glBindFramebuffer");
            glGenFramebuffers = (gl_gen_framebuffers *)glXGetProcAddress((const GLubyte*)"glGenFramebuffers");
            glFramebufferTexture2D = (gl_framebuffer_texture_2D *)glXGetProcAddress((const GLubyte*)"glFramebufferTexture2D");
            glCheckFramebufferStatus = (gl_check_framebuffer_status *)glXGetProcAddress((const GLubyte*)"glCheckFramebufferStatus");
        }

		context->glXSwapInterval =
            (glx_swap_interval_mesa *)glXGetProcAddressARB(
                (GLubyte *)"glXSwapIntervalMESA");
		if(context->glXSwapInterval)
		{
            context->glXSwapInterval(1);
		}

        glGenTextures(1, &OpenGLReservedBlitTexture);
	}
	else
	{
		InvalidCodePath;
		// TODO(casey): Diagnostic
	}

    return OpenGLContext;
}

internal hhxcb_window_dimension
hhxcbGetWindowDimension(hhxcb_context *context)
{
	hhxcb_window_dimension result;

	XGetGeometry(context->display, context->window, &result.rootWindow,
				 &result.x, &result.y,
				 &result.width, &result.height,
				 &result.borderWidth, &result.depth);

	return result;
}

internal void
hhxcbDisplayBufferInWindow(hhxcb_context *context,
						   hhxcb_offscreen_buffer *buffer,
                           platform_work_queue *RenderQueue,
                           game_render_commands *Commands,
						   rectangle2i DrawRegion,
                           memory_arena *TempArena)
{
    temporary_memory TempMem = BeginTemporaryMemory(TempArena);
    
    game_render_prep Prep = PrepForRender(Commands, TempArena);
    
/*  TODO(casey): Do we want to check for resources like before?  Probably? 
    if(AllResourcesPresent(RenderGroup))
    {
        RenderToOutput(TranState->HighPriorityQueue, RenderGroup, 
        &DrawBuffer, &TranState->TranArena);
    }
*/
    
    if(GlobalSoftwareRendering)
    {
        loaded_bitmap OutputTarget;
        OutputTarget.Memory = (uint8*)buffer->xcb_image->data;
        OutputTarget.Width = buffer->width;
		OutputTarget.Height = buffer->height;
        OutputTarget.Pitch = buffer->pitch;

        SoftwareRenderCommands(RenderQueue, Commands, &Prep, &OutputTarget, TempArena);

        OpenGLDisplayBitmap(OutputTarget.Width,
                            OutputTarget.Height,
                            OutputTarget.Memory,
                            OutputTarget.Pitch,
                            DrawRegion,
                            Commands->ClearColor,
                            OpenGLReservedBlitTexture);
            
        glXSwapBuffers(context->display, context->window);
    }
	else
	{
        BEGIN_BLOCK("OpenGLRenderCommands");
		OpenGLRenderCommands(Commands, &Prep, DrawRegion, GlobalWindowWidth, GlobalWindowHeight);
        END_BLOCK();
        
        BEGIN_BLOCK("SwapBuffers");
		glXSwapBuffers(context->display, context->window);
        END_BLOCK();
	}
    
    EndTemporaryMemory(TempMem);
}

internal void
hhxcbAddEntry(platform_work_queue* Queue, platform_work_queue_callback*
			  Callback, void* Data)
{
	// TODO: switch to "__sync_bool_compare_and_swap" so any thread can add
	uint32 NewNextEntryToWrite = (Queue->NextEntryToWrite + 1) %
		ArrayCount(Queue->Entries);
	Assert(NewNextEntryToWrite != Queue->NextEntryToRead);
	platform_work_queue_entry* Entry = Queue->Entries + Queue->NextEntryToWrite;
	Entry->Callback = Callback;
	Entry->Data = Data;
	++Queue->CompletionGoal;
	_mm_sfence();
	Queue->NextEntryToWrite = NewNextEntryToWrite;
	sem_post(Queue->SemaphoreHandle);
}

internal bool32
hhxcbDoNextWorkQueueEntry(platform_work_queue* Queue)
{
	bool32 WeShouldSleep = false;

	uint32 OriginalNextEntryToRead = Queue->NextEntryToRead;
	uint32 NewNextEntryToRead = (OriginalNextEntryToRead + 1) %
		ArrayCount(Queue->Entries);
	if(OriginalNextEntryToRead != Queue->NextEntryToWrite)
	{
		// NOTE: apparently the "__sync*" compiler intrinsics are
		// supported in gcc and clang
		if(__sync_bool_compare_and_swap(&Queue->NextEntryToRead,
										OriginalNextEntryToRead,
										(NewNextEntryToRead)))
		{
			platform_work_queue_entry Entry =
				Queue->Entries[OriginalNextEntryToRead];
			Entry.Callback(Queue, Entry.Data); 
			__sync_fetch_and_add(&Queue->CompletionCount, 1);
		}
	}
	else
	{
		WeShouldSleep = true;
	}
	return WeShouldSleep;
}

internal void
hhxcbCompleteAllWork(platform_work_queue* Queue)
{
	platform_work_queue_entry Entry = {};
    while(Queue->CompletionGoal != Queue->CompletionCount)
	{
		hhxcbDoNextWorkQueueEntry(Queue);
	}
	Queue->CompletionCount = 0;
	Queue->CompletionGoal = 0;
}

void*
ThreadFunction(void* arg)
{
    hhxcb_thread_startup *Thread = (hhxcb_thread_startup*)arg;
	platform_work_queue* Queue = Thread->Queue;

	u32 TestThreadID = GetThreadID();
    Assert(TestThreadID == (u32)pthread_self());

	for(;;)
	{
		if(hhxcbDoNextWorkQueueEntry(Queue))
		{
			sem_wait(Queue->SemaphoreHandle);
		}
	}
}

internal PLATFORM_WORK_QUEUE_CALLBACK(DoWorkerWork)
{	
	printf("thread %lu: %s\n", pthread_self(),
		   (char*)Data);
}

internal void
hhxcbMakeQueue(platform_work_queue* Queue, sem_t* SemaphoreHandle,
               uint32 ThreadCount, hhxcb_thread_startup *Startups)
{
	Queue->CompletionGoal = 0;
	Queue->CompletionCount = 0;
	
	Queue->NextEntryToWrite = 0;
	Queue->NextEntryToRead = 0;
	
	uint32 InitialCount = 0;

	// NOTE: "sem_init" has an error if it is passed
	// "Queue->SemaphoreHandle" directly
	sem_init(SemaphoreHandle, 0, InitialCount);
	Queue->SemaphoreHandle = SemaphoreHandle;
	
	for(uint32 ThreadIndex = 0;
		ThreadIndex < ThreadCount;
		++ThreadIndex)
	{
        hhxcb_thread_startup *Startup = Startups + ThreadIndex;
        Startup->Queue = Queue;
        
		pthread_t thread = {};
		pthread_attr_t attr = {};
		pthread_attr_init(&attr);
		pthread_create(&thread, &attr, &ThreadFunction, Startup);
	}	
}

struct hhxcb_platform_file_handle
{
	s32 hhxcbHandle;
};

struct hhxcb_platform_file_group
{
    dirent CurrentFileInfo;
	u32 CurrentFileNumber;
	char *SearchTerm;
	u32 SearchTermLength;
};

internal u32
hhxcbGetStringLength(char *String)
{
	u32 Result = 0;
	for(char *CharAt = String;
		*CharAt != 0;
		++CharAt)
	{
		++Result;
	}

	return Result;
}

internal char *
hhxcbConcatenateStrings(char *String1, char *String2)
{
	u32 ResultLength = hhxcbGetStringLength(String1) + hhxcbGetStringLength(String2);

	// NOTE: allocate memory for result from the os
	char *Result = (char *)mmap(
		0, (ResultLength + 1), PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);;

	char *Dest = Result;
	for(char *Source = String1;
		*Source != 0;
		)
	{
		*Dest++ = *Source++;
	}

	for(char *Source = String2;
		*Source != 0;
		)
	{
		*Dest++ = *Source++;
	}
    // NOTE: null terminate result
	*Dest = 0;

	// NOTE: return the address of result
	return Result;
}

internal u32
hhxcbGetNumberOfFilesMatched(hhxcb_platform_file_group *FileGroup)
{
	u32 Result = 0;
	
	// NOTE: iterate through all files in current directory, searching
	// for the search term to count the matches
	DIR *Directory = {};
	Directory = opendir(".");
	if(Directory)
	{
		dirent *ElementInDirectory = readdir(Directory);
		while(ElementInDirectory)
		{
			b32 Match = true;
			if(ElementInDirectory->d_type == (DT_REG | DT_LNK))
			{
				u32 NameLength = hhxcbGetStringLength(ElementInDirectory->d_name);
				if(NameLength > FileGroup->SearchTermLength)
				{
					for(s32 CharToCheck = FileGroup->SearchTermLength;
						CharToCheck >= 0;
						--CharToCheck, --NameLength)
					{
						if(ElementInDirectory->d_name[NameLength] != FileGroup->SearchTerm[CharToCheck])
						{
							Match = false;
							break;
						}
					}
					if(Match)
					{
						++Result;
					}
				}
			}
			ElementInDirectory = readdir(Directory);
		}
		closedir(Directory);
	}
	return Result;
}

internal void
hhxcbGetNextMatch(hhxcb_platform_file_group *FileGroup)
{
    // NOTE: iterate through all files in current directory, searching
	// for the search term to find the next match
	DIR *Directory = {};
	Directory = opendir(".");
	if(Directory)
	{
		u32 FileNumber = 0;

		dirent *ElementInDirectory = readdir(Directory);
		while(ElementInDirectory)
		{
			b32 Match = true;
			if(ElementInDirectory->d_type == (DT_REG | DT_LNK))
			{
				u32 NameLength = hhxcbGetStringLength(ElementInDirectory->d_name);
				if(NameLength > FileGroup->SearchTermLength)
				{
					for(s32 CharToCheck = FileGroup->SearchTermLength;
						CharToCheck >= 0;
						--CharToCheck, --NameLength)
					{
						if(ElementInDirectory->d_name[NameLength] != FileGroup->SearchTerm[CharToCheck])
						{
							Match = false;
							break;
						}	
					}
					if(Match)
					{
						if(FileNumber == FileGroup->CurrentFileNumber)
						{
							FileGroup->CurrentFileInfo = *ElementInDirectory;
							++FileGroup->CurrentFileNumber;
							break;
						}
						++FileNumber;
					}
				}
			}
			ElementInDirectory = readdir(Directory);
		}
		closedir(Directory);
	}
}

internal PLATFORM_GET_ALL_FILE_OF_TYPE_BEGIN(hhxcbGetAllFilesOfTypeBegin)
{
	platform_file_group Result = {};
	
    hhxcb_platform_file_group *hhxcbFileGroup =
		(hhxcb_platform_file_group *)mmap(
		0, sizeof(hhxcb_platform_file_group), PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	Result.Platform = hhxcbFileGroup;
	Result.FileCount = 0;
	
	hhxcbFileGroup->CurrentFileInfo = {};
	hhxcbFileGroup->CurrentFileNumber = 0;

	switch(Type)
	{
	    case PlatformFileType_AssetFile:
	    {
			hhxcbFileGroup->SearchTerm = hhxcbConcatenateStrings(".", "hha");
		}break;
		
	    case PlatformFileType_SavedGameFile:
		{
			hhxcbFileGroup->SearchTerm = hhxcbConcatenateStrings(".", "hhs");
		}break;
		
		InvalidDefaultCase;
	}
	
	hhxcbFileGroup->SearchTermLength = hhxcbGetStringLength(hhxcbFileGroup->SearchTerm);
	Result.FileCount = hhxcbGetNumberOfFilesMatched(hhxcbFileGroup);
	hhxcbGetNextMatch(hhxcbFileGroup);

    return Result;
}

internal PLATFORM_GET_ALL_FILE_OF_TYPE_END(hhxcbGetAllFilesOfTypeEnd)
{
	hhxcb_platform_file_group *hhxcbFileGroup = (hhxcb_platform_file_group *)FileGroup->Platform;
	if(hhxcbFileGroup)
	{
		// NOTE: free string allocated in hhxcbConcatenateStrings
		munmap(hhxcbFileGroup->SearchTerm, hhxcbFileGroup->SearchTermLength);
		munmap(hhxcbFileGroup, sizeof(hhxcb_platform_file_group));
	}
}
	
internal PLATFORM_OPEN_FILE(hhxcbOpenNextFile)
{
	hhxcb_platform_file_group *hhxcbFileGroup = (hhxcb_platform_file_group *)FileGroup->Platform;
	
	platform_file_handle Result = {};

	if(hhxcbFileGroup->CurrentFileInfo.d_name)
	{
		hhxcb_platform_file_handle *hhxcbFileHandle =
			(hhxcb_platform_file_handle *)mmap(
			0, sizeof(hhxcb_platform_file_handle), PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		Result.Platform = hhxcbFileHandle;

		if(hhxcbFileHandle)
		{
			char *FileName = hhxcbFileGroup->CurrentFileInfo.d_name;
			hhxcbFileHandle->hhxcbHandle = open(FileName, O_RDONLY);
			Result.NoErrors = (hhxcbFileHandle->hhxcbHandle != -1);
		}
	}

	hhxcbGetNextMatch(hhxcbFileGroup);

    return Result;
}

internal PLATFORM_FILE_ERROR(hhxcbFileError)
{
#if HANDMADE_INTERNAL
	printf("HHXCB_FILE_ERROR: ");
	printf(Message);
	printf("\n");
#endif
	Handle->NoErrors = false;
}

internal PLATFORM_READ_DATA_FROM_FILE(hhxcbReadDataFromFile)
{
	if(PlatformNoFileErrors(Source))
	{
		hhxcb_platform_file_handle *Handle = (hhxcb_platform_file_handle *)Source->Platform;
		s64 BytesRead = pread(Handle->hhxcbHandle, Dest, Size, Offset);

		if((BytesRead != -1) && ((u32)BytesRead == Size))
		{
			// NOTE: file read succeeded
		}
		else
		{
			hhxcbFileError(Source, "file read failed");
		}
	}
}

/*
internal PLATFORM_CLOSE_FILE(hhxcbCloseFile)
{
    fclose(f);
}
 */

inline b32x
hhxcbIsInLoop(hhxcb_state *state)
{
	b32x Result = ((state->recording_index != 0) ||
				   (state->playback_index));
	return(Result);
}

PLATFORM_ALLOCATE_MEMORY(hhxcbAllocateMemory)
{
    // NOTE(casey): We require memory block headers not to change the cache
	// line alignment of an allocation
	Assert(sizeof(hhxcb_memory_block) == 64);

    umm PageSize = 4096; // TODO(casey): Query from system?
	umm TotalSize = Size + sizeof(hhxcb_memory_block);
    umm BaseOffset = sizeof(hhxcb_memory_block);
    umm ProtectOffset = 0;
    if(Flags & PlatformMemory_UnderflowCheck)
	{
        TotalSize = Size + 2*PageSize;
		BaseOffset = 2*PageSize;
		ProtectOffset = PageSize;
	}
	else if(Flags & PlatformMemory_OverflowCheck)
	{
		umm SizeRoundedUp = AlignPow2(Size, PageSize);
		TotalSize = SizeRoundedUp + 2*PageSize;
		BaseOffset = PageSize + SizeRoundedUp - Size;
		ProtectOffset = PageSize + SizeRoundedUp;
	}
	
	hhxcb_memory_block *Block = (hhxcb_memory_block *)
        mmap(0, TotalSize, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    Assert(Block);
    Block->Block.Base = (u8 *)Block + BaseOffset;
    Block->AdditionalSize = TotalSize - Size;
    Assert(Block->Block.Used == 0);
    Assert(Block->Block.ArenaPrev == 0);

    if(Flags & (PlatformMemory_UnderflowCheck|PlatformMemory_OverflowCheck))
    {
        smm Protected = mprotect((u8 *)Block + ProtectOffset, PageSize, PROT_NONE);
        Assert(Protected != -1);
    }
	
	hhxcb_memory_block *Sentinel = &GlobalHhxcbState.MemorySentinel;
	Block->Next = Sentinel;
	Block->Block.Size = Size;
    Block->Block.Flags = Flags;
    Block->LoopingFlags = hhxcbIsInLoop(&GlobalHhxcbState) ? hhxcbMem_AllocatedDuringLooping : 0;
	
	BeginTicketMutex(&GlobalHhxcbState.MemoryMutex);
	Block->Prev = Sentinel->Prev;
	Block->Prev->Next = Block;
	Block->Next->Prev = Block;
	EndTicketMutex(&GlobalHhxcbState.MemoryMutex);

    platform_memory_block *PlatBlock = &Block->Block;
    return(PlatBlock);
}

PLATFORM_DEALLOCATE_MEMORY(hhxcbDeallocateMemory)
{
    if(Block)
    {
        hhxcb_memory_block *hhxcbBlock = ((hhxcb_memory_block *)Block);
        if(hhxcbIsInLoop(&GlobalHhxcbState))
		{
			hhxcbBlock->LoopingFlags = hhxcbMem_FreedDuringLooping;
		}
		else
		{
			hhxcbFreeMemoryBlock(hhxcbBlock);
		}
    }
}

#if HANDMADE_INTERNAL
global_variable debug_table GlobalDebugTable_;
debug_table *GlobalDebugTable = &GlobalDebugTable_;
#endif

int
main()
{
    DEBUGSetEventRecording(true);

    hhxcb_state *state = &GlobalHhxcbState;
    state->MemorySentinel.Prev = &state->MemorySentinel;
    state->MemorySentinel.Next = &state->MemorySentinel;
    
    hhxcb_get_binary_name(state);
    
    char mainExecutablePath[HHXCB_STATE_FILE_NAME_LENGTH];
    char *mainExecutableFilename = (char *)"xcb_handmade";
    hhxcb_build_full_filename(state, mainExecutableFilename,
            sizeof(mainExecutablePath),
            mainExecutablePath);
    
    char source_game_code_library_path[HHXCB_STATE_FILE_NAME_LENGTH];
    char *game_code_filename = (char *)"libhandmade.so";
    hhxcb_build_full_filename(state, game_code_filename,
            sizeof(source_game_code_library_path),
            source_game_code_library_path);

    hhxcb_game_code game_code = {};
    
    struct stat mainExecutableStatbuf = {};
    
    if(!stat(mainExecutablePath, &mainExecutableStatbuf))
    {
        game_code.mainExecutableMtime = mainExecutableStatbuf.st_mtime;
    }
    else
    {
        printf("couldn't stat main executable: %s\n", mainExecutablePath);
    }
    
    hhxcb_load_game(&game_code, source_game_code_library_path);
    
    DEBUGSetEventRecording(game_code.is_valid);

    hhxcb_context context = {};

    /* Open the connection to the X server. Use the DISPLAY environment variable */
//    int screenNum;
//    context.connection = xcb_connect (NULL, &screenNum);
    context.display = XOpenDisplay(0);
    context.connection = XGetXCBConnection(context.display);

    context.key_symbols = xcb_key_symbols_alloc(context.connection);

    /*
     * TODO(nbm): This is X-wide, so it really isn't a good option in reality.
     * We have to be careful and clean up at the end.  If we crash, auto-repeat
     * is left off.
     */
	// NOTE: not doing this doesn't seem to break anything
    {
        //uint32_t values[1] = {XCB_AUTO_REPEAT_MODE_OFF};
		// xcb_change_keyboard_control(context.connection, XCB_KB_AUTO_REPEAT_MODE, values);
    }

    load_atoms(&context);
    context.setup = xcb_get_setup(context.connection);
    xcb_screen_iterator_t iter = xcb_setup_roots_iterator(context.setup);
    xcb_screen_t *screen = iter.data;
    context.fmt = hhxcb_find_format(&context, 32, 24, 32);

    int monitor_refresh_hz = 60;
    real32 game_update_hz = monitor_refresh_hz; // Should almost always be an int...
    long target_nanoseconds_per_frame = (1000 * 1000 * 1000) / game_update_hz;

    uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
    uint32_t values[2] =
    {
        screen->black_pixel, //0x0000ffff,
        0
		| XCB_EVENT_MASK_POINTER_MOTION
		| XCB_EVENT_MASK_KEY_PRESS
		| XCB_EVENT_MASK_KEY_RELEASE
		| XCB_EVENT_MASK_BUTTON_PRESS
		| XCB_EVENT_MASK_BUTTON_RELEASE
		| XCB_EVENT_MASK_RESIZE_REDIRECT
		,
    };

//#define START_WIDTH 960
//#define START_HEIGHT 540
	
#define START_WIDTH 1920
#define START_HEIGHT 1080

//#define START_WIDTH 1279
//#define START_HEIGHT 719
	
    context.window = xcb_generate_id(context.connection);
	// NOTE: changed to not have a border width, so the min/max/close
	// buttons align on compiz, maybe other window managers
    xcb_create_window(context.connection, XCB_COPY_FROM_PARENT, context.window,
            screen->root, 0, 0, START_WIDTH, START_HEIGHT, 0,
            XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, mask, values);

    xcb_icccm_set_wm_name(context.connection, context.window, XCB_ATOM_STRING,
            8, strlen("hello"), "hello");

	// Note: disabling this so the default cursor is used
	//load_and_set_cursor(&context);

    xcb_map_window(context.connection, context.window);
    xcb_atom_t protocols[] =
    {
        context.wm_delete_window,
    };
    xcb_icccm_set_wm_protocols(context.connection, context.window,
            context.wm_protocols, 1, protocols);

    xcb_size_hints_t hints = {};
    xcb_icccm_size_hints_set_max_size(&hints, START_WIDTH, START_HEIGHT);
    xcb_icccm_size_hints_set_min_size(&hints, START_WIDTH, START_HEIGHT);
    xcb_icccm_set_wm_size_hints(context.connection, context.window,
            XCB_ICCCM_WM_STATE_NORMAL, &hints);

    hhxcb_offscreen_buffer buffer = {};
    hhxcb_resize_backbuffer(&context, &buffer, START_WIDTH, START_HEIGHT);

    xcb_flush(context.connection);

	GLXContext OpenGLContext = 0;
    OpenGLContext = hhxcbInitOpenGL(&context);

    hhxcb_thread_startup HighPriStartups[10] = {};
	sem_t HighQueueSemaphoreHandle = {};
	platform_work_queue HighPriorityQueue = {};
	hhxcbMakeQueue(&HighPriorityQueue, &HighQueueSemaphoreHandle, ArrayCount(HighPriStartups), HighPriStartups);

    hhxcb_thread_startup LowPriStartups[4] = {};
	sem_t LowQueueSemaphoreHandle = {};
	platform_work_queue LowPriorityQueue = {};
	hhxcbMakeQueue(&LowPriorityQueue, &LowQueueSemaphoreHandle, ArrayCount(LowPriStartups), LowPriStartups);
    
    hhxcb_sound_output sound_output = {};
    sound_output.samples_per_second = 48000;
	sound_output.channels = 2;
    sound_output.bytes_per_sample = sizeof(int16) * sound_output.channels;
	// NOTE: two second buffer
    // NOTE: do not know why the game will crash with segmentation fault
    // if this buffer is only large enough to store one second of sound
    // samples, but only when compiled with optimization enabled
	sound_output.buffer_size_in_samples = sound_output.samples_per_second * 2;
    sound_output.buffer_size_in_bytes = sound_output.buffer_size_in_samples * sound_output.bytes_per_sample;
	sound_output.safety_samples = (uint32)(((real32)sound_output.samples_per_second / game_update_hz)/3.0f);
    hhxcb_init_alsa(&context, &sound_output);

    // TODO(casey): Let's make this our first growable arena!
    memory_arena FrameTempArena = {};
    
    // TODO(casey): Decide what our pushbuffer size is!
    u32 PushBufferSize = Megabytes(64);
    platform_memory_block *PushBufferBlock = hhxcbAllocateMemory(PushBufferSize, PlatformMemory_NotRestored);
    u8 *PushBuffer = PushBufferBlock->Base;
	
    int16 *sample_buffer = (int16 *)calloc((sound_output.buffer_size_in_bytes), 1);

    game_memory m = {};

#if HANDMADE_INTERNAL
    m.DebugTable = GlobalDebugTable;
#endif
	m.HighPriorityQueue = &HighPriorityQueue;
	m.LowPriorityQueue = &LowPriorityQueue;
	m.PlatformAPI.AddEntry = hhxcbAddEntry;
	m.PlatformAPI.CompleteAllWork = hhxcbCompleteAllWork;

	m.PlatformAPI.GetAllFilesOfTypeBegin = hhxcbGetAllFilesOfTypeBegin;
	m.PlatformAPI.GetAllFilesOfTypeEnd = hhxcbGetAllFilesOfTypeEnd;
	m.PlatformAPI.OpenNextFile = hhxcbOpenNextFile;
	m.PlatformAPI.ReadDataFromFile = hhxcbReadDataFromFile;
	m.PlatformAPI.FileError = hhxcbFileError;

	m.PlatformAPI.AllocateMemory = hhxcbAllocateMemory;
	m.PlatformAPI.DeallocateMemory = hhxcbDeallocateMemory;
	
#if HANDMADE_INTERNAL
    m.PlatformAPI.DEBUGFreeFileMemory = debug_xcb_free_file_memory;
    m.PlatformAPI.DEBUGReadEntireFile = debug_xcb_read_entire_file;
    m.PlatformAPI.DEBUGWriteEntireFile = debug_xcb_write_entire_file;
	m.PlatformAPI.DEBUGExecuteSystemCommand = debug_execute_system_command;
	m.PlatformAPI.DEBUGGetProcessState = debug_get_process_state;
    m.PlatformAPI.DEBUGGetMemoryStats = hhxcbGetMemoryStats;
#endif

    u32 TextureOpCount = 1024;
    platform_texture_op_queue *TextureOpQueue = &m.TextureOpQueue;
    TextureOpQueue->FirstFree = (texture_op *)mmap
        (0, sizeof(texture_op)*TextureOpCount, PROT_WRITE|PROT_READ,
         MAP_PRIVATE|MAP_ANONYMOUS, 0, 0);
    for(u32 TextureOpIndex = 0;
        TextureOpIndex < (TextureOpCount - 1);
        ++TextureOpIndex)
    {
        texture_op *Op = TextureOpQueue->FirstFree + TextureOpIndex;
        Op->Next = TextureOpQueue->FirstFree + TextureOpIndex + 1;
    }

    Platform = m.PlatformAPI;

    timespec last_counter = hhxcbGetWallClock();

    game_input input[2] = {};
    game_input *new_input = &input[0];
    game_input *old_input = &input[1];

    int64_t next_controller_refresh = 0;
	
    while(!context.ending_flag)
    {
        // NOTE: alsa doesn't give access to the write/play cursor to do
        // proper audio debugging
        {DEBUG_DATA_BLOCK("Platform/Controls");
            DEBUG_B32(GlobalPause);
            DEBUG_B32(GlobalSoftwareRendering);
            DEBUG_B32(GlobalShowSortGroups);
        }
        
		//
		//
		//

		BEGIN_BLOCK("InputProcessing");

        game_render_commands RenderCommands = RenderCommandStruct(
            PushBufferSize, PushBuffer,
            buffer.width,
            buffer.height);
        
        // NOTE: XGetGeometry does not seem to get the right values. It just
        // seems to return the initially set values. This seems to be due
        // to xcb resize/move function (run by the window manager) not
        // setting the values after a resize
		//hhxcb_window_dimension dimension = hhxcbGetWindowDimension(&context);
        //printf("x: %d   y: %d   w: %d  h: %d\n", dimension.x, dimension.y, dimension.width, dimension.height);

        // NOTE: xcb equivalent function doesn't get the right values either
        //xcb_get_geometry_cookie_t cook = xcb_get_geometry(context.connection, context.window);
        //xcb_get_geometry_reply_t *rep = xcb_get_geometry_reply(context.connection, cook, 0);
        //printf("x: %d   y: %d   w: %d  h: %d\n", rep->x, rep->y, rep->width, rep->height);
        
        rectangle2i DrawRegion = AspectRatioFit(
            RenderCommands.Width, RenderCommands.Height,
            GlobalWindowWidth, GlobalWindowHeight);
        
        // NOTE: fixup to get opengl to draw in the right place. I think
        // this is due to an xcb bug where the window size
        // isn't updated when the window is resized. So opengl will use 
        // the initially set lower left corner of the window as it's
        // reference point, even when the window is resized and that point
        // is no longer the lower left corner of the window
        DrawRegion.MinY += START_HEIGHT - GlobalWindowHeight;
        DrawRegion.MaxY += START_HEIGHT - GlobalWindowHeight;
        
        new_input->dtForFrame = target_nanoseconds_per_frame / (1000.0 * 1000.0 * 1000.0);
        
        if (last_counter.tv_sec >= next_controller_refresh)
        {
            hhxcb_refresh_controllers(&context);
            next_controller_refresh = last_counter.tv_sec + 1;
        }

        hhxcb_process_events(&context, state, &buffer, &RenderCommands,
                             &DrawRegion, new_input, old_input);

		END_BLOCK();

		//
		//
		//

		BEGIN_BLOCK("GameUpdate");

        if(!GlobalPause)
        {
            if (state->recording_index)
            {
                hhxcb_record_input(state, new_input);
            }
            if (state->playback_index)
            {
                game_input temp = *new_input;
                hhxcb_playback_input(state, new_input);
                for(u32 MouseButtonIndex = 0;
                    MouseButtonIndex < PlatformMouseButton_Count;
                    ++MouseButtonIndex)
                {
                    new_input->MouseButtons[MouseButtonIndex] = temp.MouseButtons[MouseButtonIndex];
                }
                new_input->MouseX = temp.MouseX;
                new_input->MouseY = temp.MouseY;
                new_input->MouseZ = temp.MouseZ;
            }
            if (game_code.UpdateAndRender)
            {
                game_code.UpdateAndRender(&m, new_input, &RenderCommands);
                if(new_input->QuitRequested)
                {
                    context.ending_flag = true;
                }
            }
        }

		END_BLOCK();

		//
		//
		//

		BEGIN_BLOCK("AudioUpdate");

        if(!GlobalPause)
        {
            snd_pcm_sframes_t delay, avail;
            snd_pcm_avail_delay(context.alsa_handle, &avail, &delay);
            s32 samplesToFill = 0;
            s32 expectedSamplesPerFrame = (sound_output.samples_per_second / game_update_hz);
            s32 samplesInBuffer = sound_output.buffer_size_in_samples - avail;
            Assert(samplesInBuffer >= 0);
            // NOTE: asymptote "samplesInBuffer" to "period_size*4", since the
            // soundcard will interrupt to get samples every "period_size"
            // samples. On this machine it is ~1000 samples
            s32 sampleAdjustment = (samplesInBuffer - (sound_output.period_size*4)) / 2;
            if(sound_output.buffer_size_in_samples == avail)
            {
                // NOTE: initial fill on startup and after an underrun
                samplesToFill = (expectedSamplesPerFrame*2) + sound_output.safety_samples;
            }
            else
            {
                samplesToFill = expectedSamplesPerFrame - sampleAdjustment;
                if(samplesToFill < 0)
                {
                    samplesToFill = 0;
                }
            }
            Assert(samplesToFill >= 0);
		
            game_sound_output_buffer sound_buffer = {};
            sound_buffer.SamplesPerSecond = sound_output.samples_per_second;
            sound_buffer.SampleCount = Align8((samplesToFill));
            sound_buffer.Samples = sample_buffer;

            Assert(sound_buffer.SampleCount >= 0);
            
            if(game_code.GetSoundSamples)
            {
                game_code.GetSoundSamples(&m, &sound_buffer);
            }
            // NOTE: why don't the avail/delay values seem to work when
            // compiler optimization is enabled?
#define SOUND_DEBUG 0
#if SOUND_DEBUG
            // NOTE: "delay" is the delay of the soundcard hardware
            printf("samples in buffer before write: %ld delay in samples: %ld\n", (sound_output.buffer_size_in_samples - avail), delay);
#endif
            s32 writtenSamples = snd_pcm_writei(context.alsa_handle, sample_buffer, sound_buffer.SampleCount);
            if(writtenSamples < 0)
            {
                writtenSamples = snd_pcm_recover(context.alsa_handle, writtenSamples, 0);
            }
            else if(writtenSamples != sound_buffer.SampleCount)
            {
                printf("only wrote %d of %d samples\n", writtenSamples, sound_buffer.SampleCount);
            }

#if SOUND_DEBUG
            snd_pcm_avail_delay(context.alsa_handle, &avail, &delay);
            printf("samples in buffer after write: %ld delay in samples: %ld written samples: %d\n", (sound_output.buffer_size_in_samples - avail + writtenSamples), delay, writtenSamples);
#endif
        }
        
		END_BLOCK();

		//
		//
		//
		
#if HANDMADE_INTERNAL
		BEGIN_BLOCK("DebugCollation");

        struct stat library_statbuf = {};
        stat(source_game_code_library_path, &library_statbuf);

        b32 ExecutableNeedsToBeReloaded = (library_statbuf.st_mtime != game_code.library_mtime);

        // NOTE: main executable restart on compile
#if 0
        if(!stat(mainExecutablePath, &mainExecutableStatbuf))
        {
            b32 hhxcbNeedsToBeReloaded = 
                (mainExecutableStatbuf.st_mtime != game_code.mainExecutableMtime);
            // TODO(casey): Compare file contents here
            if(hhxcbNeedsToBeReloaded)
            {
                game_code.mainExecutableMtime = mainExecutableStatbuf.st_mtime;
                char *argv[] = {0};
                extern char **environ;
                struct stat lock;
                while(!stat("./lock.tmp", &lock))
                {
                    sleep(1);
                }
                execve(mainExecutablePath, argv, environ);
            }
        }
        else
        {
            printf("couldn't stat main executable: %s\n", mainExecutablePath);
        }
#endif

        m.ExecutableReloaded = false;
        if(ExecutableNeedsToBeReloaded)
        {
			hhxcbCompleteAllWork(&HighPriorityQueue);
			hhxcbCompleteAllWork(&LowPriorityQueue);
            DEBUGSetEventRecording(false);
        }

		if(game_code.DEBUGFrameEnd)
		{
            game_code.DEBUGFrameEnd(&m, new_input, &RenderCommands);
        }
        
        if(ExecutableNeedsToBeReloaded)
        {
            hhxcb_unload_game(&game_code);
            for(u32 LoadTryIndex = 0;
                !game_code.is_valid && (LoadTryIndex < 100);
                ++LoadTryIndex)
            {
                hhxcb_load_game(&game_code, source_game_code_library_path);
            }
			m.ExecutableReloaded = true;
            DEBUGSetEventRecording(game_code.is_valid);
		}

		END_BLOCK();
#endif
		
		//
		//
		//
		
// TODO: Leave this off until we have actual vblank support?
#if 0
		BEGIN_BLOCK("FramerateWait");

        if(!GlobalPause)
        {
            timespec target_counter = {};
            target_counter.tv_sec = last_counter.tv_sec;
            target_counter.tv_nsec = last_counter.tv_nsec + target_nanoseconds_per_frame;
            if (target_counter.tv_nsec > (1000 * 1000 * 1000))
            {
                target_counter.tv_sec++;
                target_counter.tv_nsec %= (1000 * 1000 * 1000);
            }

            timespec work_counter = hhxcbGetWallClock();

            bool32 might_need_sleep = 0;
            if (work_counter.tv_sec < target_counter.tv_sec)
            {
                might_need_sleep = 1;
            }
            else if ((work_counter.tv_sec == target_counter.tv_sec) && (work_counter.tv_nsec < target_counter.tv_nsec))
            {
                might_need_sleep = 1;
            }
            if (might_need_sleep) {
                timespec sleep_counter = {};
                sleep_counter.tv_nsec = target_counter.tv_nsec - work_counter.tv_nsec;
                if (sleep_counter.tv_nsec < 0) {
                    sleep_counter.tv_nsec += (1000 * 1000 * 1000);
                }
                // To closest ms
                sleep_counter.tv_nsec -= sleep_counter.tv_nsec % (1000 * 1000);
                if (sleep_counter.tv_nsec > 0) {
                    timespec remaining_sleep_counter = {};
                    nanosleep(&sleep_counter, &remaining_sleep_counter);
                }
                else
                {
                    // TODO(nbm): Log missed sleep
                }
            }

            timespec spin_counter = hhxcbGetWallClock();
            while (spin_counter.tv_sec <= target_counter.tv_sec && spin_counter.tv_nsec < target_counter.tv_nsec) {
                clock_gettime(HHXCB_CLOCK, &spin_counter);
            }
        }

		END_BLOCK();
#endif
		
		//
		//
		//

		BEGIN_BLOCK("FrameDisplay");

        BeginTicketMutex(&TextureOpQueue->Mutex);
        texture_op *FirstTextureOp = TextureOpQueue->First;
        texture_op *LastTextureOp = TextureOpQueue->Last;
        TextureOpQueue->Last = TextureOpQueue->First = 0;
        EndTicketMutex(&TextureOpQueue->Mutex);
					
        if(FirstTextureOp)
        {
            Assert(LastTextureOp);
            OpenGLManageTextures(FirstTextureOp);
            BeginTicketMutex(&TextureOpQueue->Mutex);
            LastTextureOp->Next = TextureOpQueue->FirstFree;
            TextureOpQueue->FirstFree = FirstTextureOp;
            EndTicketMutex(&TextureOpQueue->Mutex);
        }
        
		hhxcbDisplayBufferInWindow(&context, &buffer,
                                   &HighPriorityQueue, &RenderCommands,
                                   DrawRegion,
                                   &FrameTempArena);
		
        game_input *temp_input = new_input;
        new_input = old_input;
        old_input = temp_input;

		END_BLOCK();
		
        timespec end_counter = hhxcbGetWallClock();
		FRAME_MARKER(hhxcbGetSecondsElapsed(last_counter, end_counter));
        last_counter = end_counter;
    }

    snd_pcm_close(context.alsa_handle);

    // NOTE(nbm): Since auto-repeat seems to be a X-wide thing, let's be nice
    // and return it to where it was before?
    {
        //uint32_t values[1] = {XCB_AUTO_REPEAT_MODE_DEFAULT};
        //xcb_change_keyboard_control(context.connection, XCB_KB_AUTO_REPEAT_MODE, values);
    }

    xcb_flush(context.connection);
    xcb_disconnect(context.connection);
}
