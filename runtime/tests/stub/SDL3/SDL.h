/* Just the types overlay.h names: apputil.c includes that header, and the dialog test builds with no SDL installed. */
#ifndef STUB_SDL_H
#define STUB_SDL_H
typedef struct SDL_GPUDevice SDL_GPUDevice;
typedef struct SDL_GPUCommandBuffer SDL_GPUCommandBuffer;
typedef struct SDL_GPUTexture SDL_GPUTexture;
typedef struct SDL_Window SDL_Window;
typedef union SDL_Event SDL_Event;
typedef int SDL_GPUTextureFormat;
#endif
