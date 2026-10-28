/*
 * nativ: Project Nativ's launcher, the front door. One window shows the player's games -- every per-game pack
 * (packs/<title>/pack.toml) and every title already under dump/decrypted/ -- as cover-art cards with
 * what state each is in, and runs the pipeline's stages for them: add a game and its update from a player's own
 * copy (a PKG with its licence file, a game folder, or a whole folder searched for them), install the firmware
 * from Sony's update file once, build, play. Each card says which version the copy is and whether its game pack
 * recommends another (the pack's VERSIONS / UPDATE_NOTE: Killzone's 1.12 update brings Botzone and online play).
 * First run (or no firmware yet) opens a short setup: welcome, firmware (with Sony's official download page),
 * games, the online name, ready. The work itself is the existing command line (./vitarecomp): the hub runs it as a
 * child process, shows the step it is on, and keeps its whole output in the activity drawer -- so the command
 * line and the hub never disagree about how a step is done.
 *
 * SDL3 + Dear ImGui on SDL_GPU, the same stack as the in-game overlay (runtime/ui/overlay.cpp). Cover art is the
 * game's own sce_sys/pic0.png and icon0.png (SDL_LoadPNG). Nothing here touches a game's code or data beyond
 * reading param-like facts (the pack's NAME=, which files exist).
 *
 * ATIV_PREFS=<dir>: the launcher's own files and the shared settings there instead of the user's app folder (a test).
 * ATIV_DUMP=<dir>: the library (games, firmware, licences, builds, saves) there instead of the usual one, library_dir (a
 * fresh library to try setup on; the stages it runs inherit it). ATIV_SHOT=f.ppm (a verification aid, no visible window): renders the hub once into f.ppm and exits;
 * ATIV_SHOT_UI=library|setup:<0-4>|add|settings:<0-2>|game:<title>|sac|sac:again picks what is on screen, ATIV_SHOT_SIZE=WxH the size (1280x800),
 * ATIV_SHOT_SCAN=<rows> a folder search's results (vitarecomp scan's output) in the Add dialog.
 * ATIV_TASK=play:<title>|build:<title>|scan:<folder>|licences:<folder>|firmware:<file> (a verification aid, no window): that
 * task run as its button runs it, its log printed, the exit status its outcome; ATIV_TASK_CANCEL=<seconds>: Cancel then.
 */
#include "imgui.h"
#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_sdlgpu3.h"
#include "ui_theme.h"   /* the look shared with the in-game overlay */
#include <ctime>
#include <SDL3/SDL.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <dirent.h>
#include <fcntl.h>
#include <map>
#include <random>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <direct.h>
#include <io.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <sys/mount.h>
#include <sys/xattr.h>
#endif

#ifndef REPO_ROOT
#define REPO_ROOT "."
#endif
static std::string g_root = REPO_ROOT;   /* the repository (ATIV_ROOT overrides; else found from where the launcher is) */
static std::string g_dump;               /* the player's library: games, firmware, licences, builds, saves (library_dir) */
static const char *const SONY_FIRMWARE_PAGE = "https://www.playstation.com/en-us/support/hardware/psvita/system-software/";
static const char *const SUPPORT_PAGE = "https://ko-fi.com/kingtorro";   /* (a tip jar for the work: nothing is sold or held back) */

static bool exists(const std::string &p) { struct stat st; return stat(p.c_str(), &st) == 0; }
#ifdef _WIN32   /* (paths are UTF-8 throughout: the launcher's manifest sets the process code page, launcher/nativ.manifest) */
static void make_dir(const std::string &p) { _mkdir(p.c_str()); }
static void set_env(const char *k, const std::string &v) { _putenv_s(k, v.c_str()); }
static std::wstring wide(const std::string &s) {
	int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
	std::wstring w((size_t)n, L'\0'); if (n) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
	return w;
}
#else
static void make_dir(const std::string &p) { mkdir(p.c_str(), 0755); }
static void set_env(const char *k, const std::string &v) { setenv(k, v.c_str(), 1); }
#endif
static std::string shown_args(const std::vector<std::string> &argv) {   /* a command for the log: words with spaces quoted */
	std::string s;
	for (const std::string &a : argv) { if (!s.empty()) s += ' '; s += a.empty() || a.find_first_of(" \t\"'") != std::string::npos ? "\"" + a + "\"" : a; }
	return s;
}
/* the stages' command line (pipeline/vitarecomp.py) on the repository's Python: its virtualenv, else one installed beside the
 * launcher (<root>/python: Windows' and the self-contained Linux install's), else the system's */
static std::string python_exe(void) {
#ifdef _WIN32
	for (const char *p : {"/.venv/Scripts/python.exe", "/python/python.exe"}) if (exists(g_root + p)) return g_root + p;
	return "python";
#else
	for (const char *p : {"/.venv/bin/python", "/python/bin/python3"}) if (exists(g_root + p)) return g_root + p;   /* (an install's: <root>/python) */
	return "python3";
#endif
}
static std::vector<std::string> vitarecomp(std::initializer_list<std::string> args) {
	std::vector<std::string> v{python_exe(), g_root + "/pipeline/vitarecomp.py"};
	v.insert(v.end(), args);
	return v;
}
static void open_folder(const std::string &p) {
#ifdef _WIN32
	std::string u = p; std::replace(u.begin(), u.end(), '\\', '/'); SDL_OpenURL(("file:///" + u).c_str());
#else
	SDL_OpenURL(("file://" + p).c_str());
#endif
}

/* ---- textures: the games' own PNGs, uploaded once */
struct Tex { SDL_GPUTexture *t = nullptr; int w = 0, h = 0; };
static SDL_GPUDevice *g_gpu;
static std::map<std::string, Tex> g_tex;
static Tex texture(const std::string &path) {
	auto it = g_tex.find(path);
	if (it != g_tex.end()) return it->second;
	Tex x;
	SDL_Surface *s = exists(path) ? SDL_LoadPNG(path.c_str()) : nullptr;
	SDL_Surface *r = s ? SDL_ConvertSurface(s, SDL_PIXELFORMAT_ABGR8888) : nullptr;   /* bytes R, G, B, A */
	if (s) SDL_DestroySurface(s);
	if (r && g_gpu) {
		SDL_GPUTextureCreateInfo ci = {};
		ci.type = SDL_GPU_TEXTURETYPE_2D; ci.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM; ci.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
		ci.width = (Uint32)r->w; ci.height = (Uint32)r->h; ci.layer_count_or_depth = 1; ci.num_levels = 1;
		SDL_GPUTransferBufferCreateInfo tc = {}; tc.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD; tc.size = (Uint32)(r->w * r->h * 4);
		SDL_GPUTexture *t = SDL_CreateGPUTexture(g_gpu, &ci);
		SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(g_gpu, &tc);
		if (t && tb) {
			uint8_t *dst = (uint8_t *)SDL_MapGPUTransferBuffer(g_gpu, tb, false);
			for (int y = 0; dst && y < r->h; y++) memcpy(dst + (size_t)y * r->w * 4, (uint8_t *)r->pixels + (size_t)y * r->pitch, (size_t)r->w * 4);
			SDL_UnmapGPUTransferBuffer(g_gpu, tb);
			SDL_GPUCommandBuffer *cb = SDL_AcquireGPUCommandBuffer(g_gpu);
			SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cb);
			SDL_GPUTextureTransferInfo src = {}; src.transfer_buffer = tb; src.pixels_per_row = (Uint32)r->w; src.rows_per_layer = (Uint32)r->h;
			SDL_GPUTextureRegion dr = {}; dr.texture = t; dr.w = (Uint32)r->w; dr.h = (Uint32)r->h; dr.d = 1;
			SDL_UploadToGPUTexture(cp, &src, &dr, false);
			SDL_EndGPUCopyPass(cp); SDL_SubmitGPUCommandBuffer(cb);
			x.t = t; x.w = r->w; x.h = r->h;
		} else if (t) SDL_ReleaseGPUTexture(g_gpu, t);
		if (tb) SDL_ReleaseGPUTransferBuffer(g_gpu, tb);
	}
	if (r) SDL_DestroySurface(r);
	g_tex[path] = x;
	return x;
}
static ImTextureRef tex_ref(const Tex &t) { return ImTextureRef((ImTextureID)(intptr_t)t.t); }

/* the app icon on the window (and the Dock / taskbar): assets/brand's, macOS's drawn on its own inset grid */
static void window_icon(SDL_Window *win) {
#ifdef __APPLE__
	std::string png = g_root + "/assets/brand/png/icon-macos-512.png";
#else
	std::string png = g_root + "/assets/brand/png/icon-256.png";
#endif
	if (SDL_Surface *s = exists(png) ? SDL_LoadPNG(png.c_str()) : nullptr) { SDL_SetWindowIcon(win, s); SDL_DestroySurface(s); }
}

/* ---- the user's settings (the platform's per-user app folder, or ATIV_PREFS=<dir>: a test's): setup finished, the
 * drawer's state (hub.ini); a folder search's results (scan.tsv); the settings every game reads (settings.ini) */
static std::string pref_dir(void) {
	if (const char *d = getenv("ATIV_PREFS")) { make_dir(d); return std::string(d) + "/"; }
	char *p = SDL_GetPrefPath("ProjectNativ", "Nativ");
	std::string s = p ? p : "";
	SDL_free(p);
	/* (the name was Project Ativ: its folder's files are copied over once, before anything is read from the new one) */
	static bool moved;
	size_t at = s.rfind("ProjectNativ");
	if (!moved && at != std::string::npos) {
		moved = true;
		std::string old = s.substr(0, at) + "ProjectAtiv" + s.substr(at + 12);
		size_t leaf = old.rfind("Nativ");
		if (leaf != std::string::npos && leaf > at) old.replace(leaf, 5, "Ativ");
		for (const char *f : {"hub.ini", "settings.ini", "scan.tsv"})
			if (exists(old + f) && !exists(s + f)) {
				std::string body; if (FILE *in = fopen((old + f).c_str(), "rb")) { char b[4096]; size_t n; while ((n = fread(b, 1, sizeof b, in)) > 0) body.append(b, n); fclose(in); }
				if (FILE *out = fopen((s + f).c_str(), "wb")) { fwrite(body.data(), 1, body.size(), out); fclose(out); }
			}
	}
	return s;
}
static std::map<std::string, std::string> g_prefs;
static std::string prefs_path(void) { std::string d = pref_dir(); return d.empty() ? "" : d + "hub.ini"; }
static void prefs_load(void) {
	FILE *f = fopen(prefs_path().c_str(), "r"); if (!f) return;
	char line[512];
	while (fgets(line, sizeof line, f)) {
		char *eq = strchr(line, '='); if (!eq) continue;
		*eq = 0; std::string v = eq + 1; while (!v.empty() && (v.back() == '\n' || v.back() == '\r')) v.pop_back();
		g_prefs[line] = v;
	}
	fclose(f);
}
static void prefs_set(const char *k, const std::string &v) {
	g_prefs[k] = v;
	std::string p = prefs_path(); if (p.empty()) return;
	FILE *f = fopen(p.c_str(), "w"); if (!f) return;
	for (auto &kv : g_prefs) fprintf(f, "%s=%s\n", kv.first.c_str(), kv.second.c_str());
	fclose(f);
}

/* ---- a child process: its output (stdout and stderr together) read through a pipe; killed with everything it started
 * (POSIX: its own process group; Windows: a job object holding the process tree) */
struct Proc {
#ifdef _WIN32
	HANDLE process = nullptr, job = nullptr, out = nullptr;
#else
	pid_t pid = 0; int out = -1;
#endif
};
#ifdef _WIN32
static std::wstring win_arg(const std::wstring &a) {   /* one argument, quoted the way CommandLineToArgvW reads it back */
	if (!a.empty() && a.find_first_of(L" \t\n\v\"") == std::wstring::npos) return a;
	std::wstring q = L"\"";
	for (size_t i = 0;; i++) {
		size_t bs = 0; while (i < a.size() && a[i] == L'\\') { bs++; i++; }
		if (i == a.size()) { q.append(bs * 2, L'\\'); break; }
		if (a[i] == L'"') { q.append(bs * 2 + 1, L'\\'); q += L'"'; } else { q.append(bs, L'\\'); q += a[i]; }
	}
	return q + L"\"";
}
static bool proc_start(Proc &p, const std::vector<std::string> &argv, const std::vector<std::string> &env) {
	SECURITY_ATTRIBUTES sa = {sizeof sa, nullptr, TRUE};
	HANDLE rd, wr, nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
	if (!CreatePipe(&rd, &wr, &sa, 0)) return false;
	SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
	std::wstring cmd; for (const std::string &a : argv) { if (!cmd.empty()) cmd += L' '; cmd += win_arg(wide(a)); }
	std::wstring block;   /* the launcher's environment with the step's NAME=value settings over it */
	if (!env.empty()) {
		std::vector<std::wstring> vars;
		if (wchar_t *e = GetEnvironmentStringsW()) { for (wchar_t *v = e; *v; v += wcslen(v) + 1) vars.push_back(v); FreeEnvironmentStringsW(e); }
		for (const std::string &kv : env) {
			std::wstring w = wide(kv), k = w.substr(0, w.find(L'=') + 1);
			vars.erase(std::remove_if(vars.begin(), vars.end(), [&](const std::wstring &v) { return _wcsnicmp(v.c_str(), k.c_str(), k.size()) == 0; }), vars.end());
			vars.push_back(w);
		}
		for (const std::wstring &v : vars) { block += v; block += L'\0'; }
		block += L'\0';
	}
	STARTUPINFOW si = {}; si.cb = sizeof si; si.dwFlags = STARTF_USESTDHANDLES; si.hStdInput = nul; si.hStdOutput = wr; si.hStdError = wr;
	PROCESS_INFORMATION pi = {};
	std::wstring dir = wide(g_root);
	BOOL ok = CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, TRUE, CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
	                         env.empty() ? nullptr : &block[0], dir.c_str(), &si, &pi);
	CloseHandle(wr); if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
	if (!ok) { CloseHandle(rd); return false; }
	p.job = CreateJobObjectW(nullptr, nullptr);
	if (p.job) AssignProcessToJobObject(p.job, pi.hProcess);
	ResumeThread(pi.hThread); CloseHandle(pi.hThread);
	p.process = pi.hProcess; p.out = rd;
	return true;
}
static long proc_read(Proc &p, char *buf, size_t n) { DWORD got = 0; return ReadFile(p.out, buf, (DWORD)n, &got, nullptr) ? (long)got : -1; }
static int proc_wait(Proc &p) {   /* its exit code (a Cancel: 1) */
	WaitForSingleObject(p.process, INFINITE);
	DWORD code = 1; GetExitCodeProcess(p.process, &code);
	CloseHandle(p.out); CloseHandle(p.process);
	return (int)code;
}
static void proc_kill(Proc &p) { if (p.job) TerminateJobObject(p.job, 1); else if (p.process) TerminateProcess(p.process, 1); }
static void proc_done(Proc &p) { if (p.job) CloseHandle(p.job); p = Proc(); }
#else
extern char **environ;
static bool proc_start(Proc &p, const std::vector<std::string> &argv, const std::vector<std::string> &env) {
	/* everything the child needs made before fork: the launcher has threads, and a child of one may only exec */
	std::vector<char *> a; for (const std::string &s : argv) a.push_back((char *)s.c_str()); a.push_back(nullptr);
	std::vector<std::string> vars;
	for (char **e = environ; *e; e++) {
		bool over = false;
		for (const std::string &kv : env) over |= strncmp(*e, kv.c_str(), kv.find('=') + 1) == 0;
		if (!over) vars.push_back(*e);
	}
	vars.insert(vars.end(), env.begin(), env.end());
	std::vector<char *> envp; for (std::string &v : vars) envp.push_back(&v[0]); envp.push_back(nullptr);
	int fd[2];
	if (pipe(fd) != 0) return false;
	pid_t pid = fork();
	if (pid == 0) {
		setpgid(0, 0);
		dup2(fd[1], 1); dup2(fd[1], 2); close(fd[0]); close(fd[1]);
		if (chdir(g_root.c_str()) != 0) _exit(126);
		environ = envp.data();
		execvp(a[0], a.data());
		static const char msg[] = "can't run the command\n"; (void)!write(2, msg, sizeof msg - 1);
		_exit(127);
	}
	close(fd[1]);
	if (pid < 0) { close(fd[0]); return false; }
	setpgid(pid, pid);
	p.pid = pid; p.out = fd[0];
	return true;
}
static long proc_read(Proc &p, char *buf, size_t n) { return (long)read(p.out, buf, n); }
static int proc_wait(Proc &p) {   /* its exit code; killed by a signal: 128 + the signal */
	close(p.out);
	int st = 0; waitpid(p.pid, &st, 0);
	return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}
static void proc_kill(Proc &p) { if (p.pid > 0) kill(-p.pid, SIGTERM); }
static void proc_done(Proc &p) { p = Proc(); }
#endif

/* ---- one task at a time: its steps run in turn, their output into the log; Cancel kills the step running and runs no
 * more. A step announces itself with an "@@ <step>" line (the latest is the status). A failed step ends its group: group
 * 0 is the whole task; a folder search's chain gives each game its own (one game failing doesn't stop the next) */
struct Step {
	std::string say;                 /* "@@ " + this before it runs (a step with no command: only the line) */
	std::vector<std::string> argv;
	std::vector<std::string> env;    /* NAME=value over the launcher's environment */
	int group = 0;
};
/* ---- a newer Nativ: `vitarecomp update-check` in the background at start (it asks GitHub at most once a day, and only
 * an install whose dist named a release channel); its release, or nothing */
static struct { std::mutex m; std::string tag, page; bool dismissed = false; } g_update;
static void update_check_start(void) {
	std::thread([]() {
		Proc p; if (!proc_start(p, vitarecomp({"update-check"}), {})) return;
		std::string out; char b[512]; long n; while ((n = proc_read(p, b, sizeof b)) > 0) out.append(b, (size_t)n);
		proc_wait(p); proc_done(p);
		for (size_t a = 0; a < out.size(); ) {
			size_t e = out.find('\n', a); if (e == std::string::npos) e = out.size();
			char tag[64], page[512];
			if (sscanf(out.substr(a, e - a).c_str(), "update %63s %511s", tag, page) == 2 && !strncmp(page, "https://", 8)) {
				std::lock_guard<std::mutex> l(g_update.m); g_update.tag = tag; g_update.page = page;
			}
			a = e + 1;
		}
	}).detach();
}
static Step step(const std::string &say, std::vector<std::string> argv = {}, int group = 0) { Step s; s.say = say; s.argv = std::move(argv); s.group = group; return s; }
struct Task {
	std::string name, game, kind;
	std::vector<std::string> games;   /* the titles it works on: their cards show its step (game, or a search's chain's) */
	std::atomic<bool> running{false}, cancelled{false};
	bool ok = false;
	size_t log_from = 0;
	std::thread reader;
	std::mutex proc_lock; Proc proc;  /* the step running */
};
static Task g_task;
static std::mutex g_log_lock;
static std::string g_log;
static std::atomic<bool> g_refresh{false};   /* a task ended: the library's states may have changed */
static std::string g_failed;                  /* the last task that failed, for the banner (empty: none) */
static bool g_drawer;                         /* the activity drawer open (the whole log) */
static std::string g_log_file;                /* <prefs>/launcher.log: the log as each task ends, for a bug report (the launcher
                                               * before this one: launcher.prev.log) */

static void log_append(const char *s, size_t n) {
	std::lock_guard<std::mutex> l(g_log_lock);
	g_log.append(s, n);
	if (g_log.size() > (1u << 20)) { size_t cut = g_log.size() - (1u << 19); g_log.erase(0, cut); g_task.log_from = g_task.log_from > cut ? g_task.log_from - cut : 0; }
}
static void log_append(const std::string &s) { log_append(s.data(), s.size()); }
static void task_start(const std::string &name, const std::string &kind, const std::string &game, std::vector<Step> steps) {
	if (g_task.running) return;
	if (g_task.reader.joinable()) g_task.reader.join();
	g_failed.clear();
	{ std::lock_guard<std::mutex> l(g_log_lock); g_log += "\n== " + name + "\n"; g_task.log_from = g_log.size(); }
	g_task.name = name; g_task.kind = kind; g_task.game = game; g_task.ok = false; g_task.cancelled = false; g_task.running = true;
	g_task.games.clear(); if (!game.empty()) g_task.games.push_back(game);
	g_task.reader = std::thread([steps = std::move(steps), name]() {
		std::vector<int> failed;   /* the groups that failed */
		int last = 0;
		for (const Step &s : steps) {
			if (g_task.cancelled) break;
			if (std::find(failed.begin(), failed.end(), s.group) != failed.end()) continue;
			if (!s.say.empty()) log_append("@@ " + s.say + "\n");
			if (s.argv.empty()) continue;
			log_append("$ " + shown_args(s.argv) + "\n");
			Proc p;
			{ std::lock_guard<std::mutex> l(g_task.proc_lock); if (g_task.cancelled || !proc_start(p, s.argv, s.env)) { if (!g_task.cancelled) log_append("can't start " + s.argv[0] + "\n"); last = 127; failed.push_back(s.group); if (s.group == 0) break; continue; } g_task.proc = p; }
			char buf[4096]; long n;
			while ((n = proc_read(p, buf, sizeof buf)) > 0) log_append(buf, (size_t)n);
			int code = proc_wait(p);
			{ std::lock_guard<std::mutex> l(g_task.proc_lock); proc_done(g_task.proc); }
			if (code != 0) { last = code; failed.push_back(s.group); if (s.group == 0) break; }
		}
		bool stopped = g_task.cancelled;
		g_task.ok = failed.empty() && !stopped;
		char end[96]; int m = snprintf(end, sizeof end, "== %s (exit %d)\n", stopped ? "stopped" : "done", stopped ? -1 : last);
		log_append(end, (size_t)m);
		if (!g_task.ok && !stopped) g_failed = name;
		if (!g_log_file.empty()) {
			std::string copy; { std::lock_guard<std::mutex> l(g_log_lock); copy = g_log; }
			if (FILE *f = fopen(g_log_file.c_str(), "wb")) { fwrite(copy.data(), 1, copy.size(), f); fclose(f); }
		}
		g_task.running = false;
		g_refresh = true;
	});
}
static void task_cancel(void) {
	if (!g_task.running) return;
	std::lock_guard<std::mutex> l(g_task.proc_lock);
	g_task.cancelled = true;
	proc_kill(g_task.proc);
}
static std::string task_status(void) {   /* the latest "@@ step" of the running task, else its last line of output */
	std::lock_guard<std::mutex> l(g_log_lock);
	size_t from = g_task.log_from < g_log.size() ? g_task.log_from : g_log.size();
	size_t at = g_log.rfind("\n@@ ");
	if (at != std::string::npos && at + 1 >= from) { size_t e = g_log.find('\n', at + 4); return g_log.substr(at + 4, e == std::string::npos ? std::string::npos : e - at - 4); }
	size_t e = g_log.size(); while (e > from && (g_log[e - 1] == '\n' || g_log[e - 1] == ' ')) e--;
	size_t b = g_log.rfind('\n', e ? e - 1 : 0); b = b == std::string::npos || b < from ? from : b + 1;
	std::string s = e > b ? g_log.substr(b, e - b) : "";
	if (s.size() > 90) s = s.substr(0, 87) + "...";
	return s;
}

/* ---- the library: packs + whatever is under dump/decrypted */
struct Game {
	std::string id, name, elf = "eboot.bin.elf", base = "game";   /* base: the pack's BASE= (the host binary is <base>_host) */
	std::string version;      /* this copy's (dump/decrypted/<id>/version, vitarecomp import's); "-": the pack's one build, no number recorded */
	std::vector<std::pair<std::string, std::string>> versions;   /* the pack's versions: (version, recommended | supported) */
	std::string update_note;  /* the pack's UPDATE_NOTE: what its recommended version brings */
	std::string cover, icon;  /* the copy's own art (art_paths): "" when it has none */
	std::string region;       /* the pack's REGION: the release it plays (US) */
	bool pack = false, imported = false, built = false;
};
static std::string recommended_version(const Game &g) {
	for (auto &v : g.versions) if (v.second == "recommended") return v.first;
	return "";
}
static std::string played_versions(const Game &g) {   /* "1.00, 1.12" (the numbers recorded) */
	std::string s;
	for (auto &v : g.versions) if (v.first != "-") s += (s.empty() ? "" : ", ") + v.first;
	return s;
}
static std::vector<Game> g_games;
static bool g_firmware;
static std::string g_firmware_version;

/* a game pack (packs/<title>/pack.toml; the format: pipeline/packconf.py), as far as the launcher reads it: the top-level
 * settings (KEY = "text" or a number) and each [versions."<version>"] table's status, in the pack's order */
struct Pack { std::map<std::string, std::string> top; std::vector<std::pair<std::string, std::string>> versions; };
static Pack pack_read(const std::string &path) {
	Pack p; std::string text, table;
	if (FILE *f = fopen(path.c_str(), "rb")) { char b[4096]; size_t n; while ((n = fread(b, 1, sizeof b, f)) > 0) text.append(b, n); fclose(f); }
	for (size_t at = 0; at < text.size();) {
		size_t end = text.find('\n', at); if (end == std::string::npos) end = text.size();
		const std::string l = text.substr(at, end - at); at = end + 1;
		size_t s = l.find_first_not_of(" \t\r"); if (s == std::string::npos || l[s] == '#') continue;
		if (l[s] == '[') {
			size_t e = l.rfind(']'); table = e != std::string::npos && e > s ? l.substr(s + 1, e - s - 1) : "";
			if (table.size() > 11 && table.compare(0, 10, "versions.\"") == 0 && table.back() == '"') p.versions.push_back({table.substr(10, table.size() - 11), "supported"});
			continue;
		}
		size_t eq = l.find('=', s), v = eq == std::string::npos ? eq : l.find_first_not_of(" \t", eq + 1);
		if (eq == std::string::npos || eq == s || v == std::string::npos) continue;
		const std::string key = l.substr(s, l.find_last_not_of(" \t", eq - 1) - s + 1);
		std::string val;
		if (l[v] == '"') { for (v++; v < l.size() && l[v] != '"'; v++) { if (l[v] == '\\' && v + 1 < l.size()) v++; val += l[v]; } }
		else { size_t e = l.find_first_of(" \t#\r", v); val = l.substr(v, e == std::string::npos ? std::string::npos : e - v); }
		if (table.empty()) p.top[key] = val;
		else if (key == "status" && table.compare(0, 9, "versions.") == 0 && !p.versions.empty()) p.versions.back().second = val;
	}
	return p;
}
static std::string pack_value(const Pack &p, const char *key) { auto it = p.top.find(key); return it == p.top.end() ? "" : it->second; }
static std::string first_line(const std::string &path) {
	FILE *f = fopen(path.c_str(), "r"); if (!f) return "";
	char line[256] = ""; if (!fgets(line, sizeof line, f)) line[0] = 0;
	fclose(f);
	std::string v = line; while (!v.empty() && (v.back() == '\n' || v.back() == '\r' || v.back() == ' ')) v.pop_back();
	return v;
}
static bool title_like(const char *s) {   /* PCSA00029: four letters, five digits */
	if (strlen(s) != 9) return false;
	for (int i = 0; i < 4; i++) if (s[i] < 'A' || s[i] > 'Z') return false;
	for (int i = 4; i < 9; i++) if (s[i] < '0' || s[i] > '9') return false;
	return true;
}
static void art_paths(Game &g);
static void count_licences(void);
static void library_scan(void) {
	count_licences();
	for (auto it = g_tex.begin(); it != g_tex.end();) it = it->second.t ? std::next(it) : g_tex.erase(it);   /* (retry art not found before) */
	std::map<std::string, Game> by;
	std::string packs = g_root + "/packs";
	if (DIR *d = opendir(packs.c_str())) {
		while (dirent *e = readdir(d)) {
			std::string n = e->d_name;
			const std::string path = packs + "/" + n + "/pack.toml";
			if (n.size() != 9 || !title_like(n.c_str()) || !exists(path)) continue;
			const Pack p = pack_read(path);
			Game &g = by[n];
			g.id = n; g.pack = true;
			g.name = pack_value(p, "NAME");
			std::string elf = pack_value(p, "ELF"); if (!elf.empty()) g.elf = elf;
			std::string base = pack_value(p, "BASE"); if (!base.empty()) g.base = base;
			g.versions = p.versions;
			g.update_note = pack_value(p, "UPDATE_NOTE");
			g.region = pack_value(p, "REGION");
		}
		closedir(d);
	}
	std::string dumps = g_dump + "/decrypted";
	if (DIR *d = opendir(dumps.c_str())) {
		while (dirent *e = readdir(d)) {
			if (!title_like(e->d_name)) continue;
			std::string w = dumps + "/" + e->d_name;
			if (!exists(w + "/elf")) continue;   /* a scratch or analysis folder, not an imported game */
			Game &g = by[e->d_name]; g.id = e->d_name;
		}
		closedir(d);
	}
	g_games.clear();
	for (auto &kv : by) {
		Game g = kv.second;
		std::string w = dumps + "/" + g.id;
		g.imported = exists(w + "/elf/" + g.elf);
#ifdef _WIN32
		g.built = exists(w + "/recomp/" + g.base + "_host.exe");
#else
		g.built = exists(w + "/recomp/" + g.base + "_host");
#endif
		g.version = first_line(w + "/version");
		if (g.imported) art_paths(g);
		if (g.name.empty()) g.name = g.id;
		g_games.push_back(g);
	}
	/* a console dump's modules, or a firmware update file's from setup (dump/firmware/<version>/) */
	g_firmware = exists(dumps + "/us/libgxm_es4.suprx.elf"); g_firmware_version.clear();
	std::string fw = g_dump + "/firmware";
	if (DIR *d = opendir(fw.c_str())) {   /* the newest update file installed */
		while (dirent *e = readdir(d))
			if (e->d_name[0] != '.' && exists(fw + "/" + e->d_name + "/os0/us/libgxm_es4.suprx.elf") && std::string(e->d_name) > g_firmware_version) { g_firmware = true; g_firmware_version = e->d_name; }
		closedir(d);
	}
	if (g_firmware && g_firmware_version.empty()) g_firmware_version = "from a console dump";
}
/* ---- a card's art, from the copy's own sce_sys: pic0.png (960x544, the game's background picture); else its LiveArea's
 * background (template.xml <livearea-background><image>, 840x500); else its LiveArea gate's start image (280x158).
 * (Nothing of a game ships with Nativ: a game not added yet keeps its typeset placeholder.) */
static std::string read_file(const std::string &path) {
	std::string s; FILE *f = fopen(path.c_str(), "rb"); if (!f) return s;
	char buf[4096]; size_t n; while ((n = fread(buf, 1, sizeof buf, f)) > 0 && s.size() < (1u << 20)) s.append(buf, n);
	fclose(f);
	return s;
}
static std::string tag_text(const std::string &x, const char *tag, size_t from = 0) {   /* the text right after <tag ...>, trimmed */
	size_t a = x.find(tag, from); if (a == std::string::npos) return "";
	a = x.find('>', a); if (a == std::string::npos) return "";
	size_t b = x.find('<', ++a); std::string v = x.substr(a, b == std::string::npos ? std::string::npos : b - a);
	size_t s0 = v.find_first_not_of(" \t\r\n"), s1 = v.find_last_not_of(" \t\r\n");
	return s0 == std::string::npos ? "" : v.substr(s0, s1 - s0 + 1);
}
static void art_paths(Game &g) {
	std::string sys = g_dump + "/decrypted/" + g.id + "/recomp/fs/app0/sce_sys/", la = sys + "livearea/contents/";
	g.icon = exists(sys + "icon0.png") ? sys + "icon0.png" : "";
	g.cover.clear();
	if (exists(sys + "pic0.png")) { g.cover = sys + "pic0.png"; return; }
	std::string x = read_file(la + "template.xml");
	size_t bg = x.find("<livearea-background");
	for (const std::string &v : {bg == std::string::npos ? std::string() : tag_text(x, "<image", bg), tag_text(x, "<startup-image")})
		if (!v.empty() && v.find('/') == std::string::npos && exists(la + v)) { g.cover = la + v; return; }
}

/* ---- the native file dialogs (SDL3): their callback lands the chosen path in a field */
static std::mutex g_pick_lock;
static std::string *g_pick_target;
static std::string g_pick_result; static bool g_pick_done;
static void SDLCALL picked(void *, const char *const *files, int) {
	std::lock_guard<std::mutex> l(g_pick_lock);
	if (files && files[0]) { g_pick_result = files[0]; g_pick_done = true; }
}
static void pick_file(SDL_Window *w, std::string *target, const SDL_DialogFileFilter *f, int nf) { g_pick_target = target; SDL_ShowOpenFileDialog(picked, nullptr, w, f, nf, nullptr, false); }
static void pick_folder(SDL_Window *w, std::string *target) { g_pick_target = target; SDL_ShowOpenFolderDialog(picked, nullptr, w, nullptr, false); }
static void pick_poll(void) {
	std::lock_guard<std::mutex> l(g_pick_lock);
	if (g_pick_done && g_pick_target) *g_pick_target = g_pick_result;
	g_pick_done = false;
}
static bool input_text(const char *label, std::string &s, float w = -1, const char *hint = nullptr) {   /* ImGui::InputText over a std::string */
	char buf[2048]; snprintf(buf, sizeof buf, "%s", s.c_str());
	ImGui::SetNextItemWidth(w);
	bool ch = hint ? ImGui::InputTextWithHint(label, hint, buf, sizeof buf) : ImGui::InputText(label, buf, sizeof buf);
	if (ch) s = buf;
	return ch;
}
static bool path_row(const char *id, std::string &s, const char *hint) {   /* a path field with Browse... beside it: true when Browse is pressed */
	float bw = ImGui::CalcTextSize("Browse…").x + ImGui::GetStyle().FramePadding.x * 2;
	input_text(id, s, -bw - ImGui::GetStyle().ItemSpacing.x, hint);
	ImGui::SameLine();
	ImGui::PushID(id); bool r = ImGui::Button("Browse…"); ImGui::PopID();
	return r;
}

/* ---- the stages, as commands */
static void build_steps(std::vector<Step> &steps, const std::string &id, int group = 0) {   /* analyse, recompile, build: each its own "@@" step */
	steps.push_back(step("Step 1 of 3: analysing the game", vitarecomp({"analyze", id}), group));
	steps.push_back(step("Step 2 of 3: recompiling it to C", vitarecomp({"recompile", id}), group));
	steps.push_back(step("Step 3 of 3: compiling the native program", vitarecomp({"build", id}), group));
	steps.push_back(step("Ready to play", {}, group));
}
static void start_build(const Game &g) { std::vector<Step> s; build_steps(s, g.id); task_start("Building " + g.name, "build", g.id, s); }
static bool needs_online_name(const Game &g);   /* (below: an online game, and no name of the player's own yet) */
static std::string g_name_game;                  /* the game waiting on the online name prompt (online_name_modal) */
static void start_play_now(const Game &g);
static void start_play(const Game &g) {
	if (needs_online_name(g)) { g_name_game = g.id; return; }
	start_play_now(g);
}
static void start_play_now(const Game &g) {   /* (ATIV_LAUNCHER: the game's menu offers "Back to the library"; the launcher comes back to the front when it ends) */
	/* ready first: a build older than these tools (Nativ was updated) is brought up to date before the game starts, so an
	 * update reaches every game; when it's current that's a moment */
	Step r = step("Checking " + g.name + "'s build is up to date", vitarecomp({"ready", g.id}));
	Step s = step("Running " + g.name, vitarecomp({"run", g.id})); s.env.push_back("ATIV_LAUNCHER=1");
	task_start("Playing " + g.name, "play", g.id, {r, s});
}

/* ---- a controller's L1 / R1 (or Ctrl+Tab, Ctrl+Shift+Tab) step a row of tabs: -1, +1, or 0 -- not while a control is held */
static int tab_step(void) {
	if (ImGui::IsAnyItemActive()) return 0;
	const bool ctrl = ImGui::GetIO().KeyCtrl, shift = ImGui::GetIO().KeyShift;
	if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) || (ctrl && shift && ImGui::IsKeyPressed(ImGuiKey_Tab, false))) return -1;
	if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) || (ctrl && !shift && ImGui::IsKeyPressed(ImGuiKey_Tab, false))) return 1;
	return 0;
}
static void tab_hint(void) {   /* (shown with a controller connected) */
	if (SDL_HasGamepad()) { ImGui::SameLine(); ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("L1 / R1"); }
}

/* ---- shared pieces of the setup and the dialogs */
static int g_add_kind = 0;   /* 0 a folder to search, 1 a PKG file, 2 a game folder */
static std::string g_add_path, g_add_license, g_fw_path, g_scan_path;

/* "Where's my licence?": a PKG carries none (PSN gives each owner their own, the PKG is the same for everyone); the
 * owner's Vita has it -- a game installed from a PKG keeps it in its folder, sce_sys/package/work.bin; a game bought
 * and installed the usual way gets one written by the NoNpDrm plugin when started, ux0:nonpdrm/license/app/.
 * Searching a folder that holds the licence file finds it by its content id. */
static void center_next_popup(void);
static void licence_help_popup(void) {
	center_next_popup();
	if (!ImGui::BeginPopup("licence_help")) return;
	const float em = ImGui::GetFontSize();
	ImGui::PushTextWrapPos(em * 30);
	ImGui::TextUnformatted("Where's my licence?");
	ui::muted_wrapped("A PKG never carries one: PSN keeps a licence for each owner, and the PKG is the same file for everyone. Your Vita has yours:");
	ImGui::Bullet(); ImGui::TextWrapped("A game installed from its PKG: its folder holds the licence, ux0:app/<title>/sce_sys/package/work.bin. Copy the whole game folder and nothing else is needed.");
	ImGui::Bullet(); ImGui::TextWrapped("A game bought and installed the usual way: with the NoNpDrm plugin on, start the game once. Its licence is then saved as a .rif file in ux0:nonpdrm/license/app/<title>/.");
	ImGui::Bullet(); ImGui::TextWrapped("Copy that file (or the whole ux0:nonpdrm folder) next to your PKG and search the folder: Nativ matches each licence to its game.");
	ui::muted_wrapped("Easiest: Licences, From my Vita… fetches them all at once over Wi-Fi. Nativ keeps your licences on this computer only.");
	ImGui::PopTextWrapPos();
	ImGui::EndPopup();
}
static void licence_help_link(void) { if (ImGui::TextLink("Where's my licence?")) ImGui::OpenPopup("licence_help"); licence_help_popup(); }

/* ---- the player's licences: how many are kept, and three ways in -- the Vita itself (VitaShell's FTP), a folder (a
 * copied ux0:nonpdrm, licence files kept anywhere), a Vita3K install's -- each once; a folder search keeps the ones it
 * finds too. Fetched or imported, every later PKG, update and add-on of those games is added without asking. */
static int g_licences;   /* kept, counted at each library scan */
static std::string g_vita_addr, g_lic_folder;
static bool g_open_vita_popup;   /* (ATIV_SHOT_UI=add:vita: the popup open in a screenshot) */
static std::string vita3k_licences(void) {   /* Vita3K's ux0/license where it keeps it by default (SDL's pref path Vita3K/Vita3K), "" none */
	const char *home = getenv("HOME"), *appdata = getenv("APPDATA"), *xdg = getenv("XDG_DATA_HOME");
	std::vector<std::string> bases;
	if (home) { bases.push_back(std::string(home) + "/Library/Application Support/Vita3K/Vita3K"); bases.push_back((xdg ? std::string(xdg) : std::string(home) + "/.local/share") + "/Vita3K/Vita3K"); }
	if (appdata) bases.push_back(std::string(appdata) + "/Vita3K/Vita3K");
	for (const std::string &b : bases) for (const char *sub : {"/ux0/license", "/fs/ux0/license"}) if (exists(b + sub)) return b + sub;
	return "";
}
static void count_licences(void) {
	g_licences = 0;
	if (DIR *d = opendir((g_dump + "/licenses").c_str())) { while (dirent *e = readdir(d)) { size_t n = strlen(e->d_name); g_licences += n > 4 && !strcmp(e->d_name + n - 4, ".rif"); } closedir(d); }
}
static void center_next_popup(void) { const ImGuiViewport *vp = ImGui::GetMainViewport(); ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f)); }
static void vita_licences_popup(void) {
	center_next_popup();
	if (!ImGui::BeginPopup("vita_licences")) return;
	const float em = ImGui::GetFontSize();
	ImGui::PushTextWrapPos(em * 28);
	ImGui::TextUnformatted("Get your licences from your Vita");
	ui::muted_wrapped("Your Vita and this computer on the same network. Nativ reads only licence files, nothing else.");
	ImGui::TextWrapped("1. On the Vita, open VitaShell and press SELECT. It shows an address like ftp://192.168.0.50:1337.");
	ImGui::TextWrapped("2. Type that address here:");
	if (g_vita_addr.empty()) g_vita_addr = g_prefs["vita_address"];
	input_text("##vaddr", g_vita_addr, em * 16, "192.168.0.50:1337");
	ImGui::SameLine();
	ImGui::BeginDisabled(g_vita_addr.empty() || g_task.running);
	if (ui::primary_button("Get licences")) {
		std::string a = g_vita_addr; if (a.rfind("ftp://", 0) == 0) a = a.substr(6); while (!a.empty() && a.back() == '/') a.pop_back();
		prefs_set("vita_address", a);
		task_start("Getting licences from the Vita", "licences", "", {step("Reading the Vita", vitarecomp({"licenses", "vita", a}))});
		ImGui::CloseCurrentPopup();
	}
	ImGui::EndDisabled();
	ui::muted_wrapped("A game bought and installed the usual way has a licence only the Vita can use: start it once with the NoNpDrm plugin on, then get licences again.");
	ImGui::PopTextWrapPos();
	ImGui::EndPopup();
}
static void licences_row(SDL_Window *win) {
	ImGui::PushID("licences");
	ui::dot(g_licences ? ui::ready : ui::border);
	ImGui::Text("Licences: %d kept", g_licences); ImGui::SameLine();
	ImGui::BeginDisabled(g_task.running);
	if (ImGui::SmallButton("From my Vita…") || g_open_vita_popup) { ImGui::OpenPopup("vita_licences"); g_open_vita_popup = false; }
	ImGui::SameLine(); if (ImGui::SmallButton("From a folder…")) { g_lic_folder.clear(); pick_folder(win, &g_lic_folder); }
	std::string v3k = vita3k_licences();
	if (!v3k.empty()) { ImGui::SameLine(); if (ImGui::SmallButton("From Vita3K")) task_start("Importing Vita3K's licences", "licences", "", {step("", vitarecomp({"licenses", "vita3k", v3k}))}); }
	ImGui::EndDisabled();
	if (!g_lic_folder.empty() && !g_task.running) {   /* (a folder picked: its licences in) */
		task_start("Importing licences", "licences", "", {step("", vitarecomp({"licenses", "import", g_lic_folder}))});
		g_lic_folder.clear();
	}
	vita_licences_popup();
	ImGui::PopID();
}

/* a folder search's finds (vitarecomp scan: pipeline/packs.py's rows) -- games, updates, add-ons, firmware files, each
 * with what its game pack says of that version -- picked for adding in one go */
struct Found { std::string kind, form, id, version, status, licence, name, path, licence_file, content_id, note; bool pick = false; };
/* the player's licences, kept once (pipeline/licenses.py: <dump>/licenses/<content id>.rif): add finds a copy's
 * there by itself, so a row whose licence is kept -- by this search, or fetched from the Vita meanwhile -- is ready */
static std::string kept_licence(const std::string &cid) { std::string p = g_dump + "/licenses/" + cid + ".rif"; return !cid.empty() && cid != "-" && exists(p) ? p : ""; }
static std::vector<Found> g_found;
static bool g_found_ready;   /* a search finished: its rows (maybe none) are in g_found */
static std::string scan_file(void) { std::string d = pref_dir(); return d.empty() ? g_dump + "/.scan.tsv" : d + "scan.tsv"; }
static bool playable_status(const std::string &st) { return st == "recommended" || st == "supported" || st == "check"; }
static void scan_load(const std::string &path) {
	g_found.clear(); g_found_ready = true;
	FILE *f = fopen(path.c_str(), "r"); if (!f) return;
	char line[4096];
	while (fgets(line, sizeof line, f)) {
		std::vector<std::string> c(1);
		for (const char *q = line; *q && *q != '\n' && *q != '\r'; q++) { if (*q == '\t') c.emplace_back(); else c.back() += *q; }
		if (c.size() < 8) continue;
		Found x; x.kind = c[0]; x.form = c[1]; x.id = c[2]; x.version = c[3]; x.status = c[4]; x.licence = c[5]; x.name = c[6]; x.path = c[7];
		if (c.size() > 8) x.licence_file = c[8];   /* (licence "file": a kept one, or one found elsewhere in the folder) */
		if (c.size() > 9) x.content_id = c[9];
		if (c.size() > 10) x.note = c[10];   /* (a copy Nativ can't play as it is: which release and version it does) */
		/* picked to start with: what plays and needs nothing typed in; the firmware when none is installed (the newest file) */
		if (x.kind == "game" || x.kind == "update") {
			bool dup = false; for (const Found &y : g_found) dup |= y.pick && y.kind == x.kind && y.id == x.id;
			x.pick = !dup && playable_status(x.status);   /* (one still needing its licence is ticked, its field showing; it's added once it has one) */
		} else if (x.kind == "firmware" && x.status == "firmware" && !g_firmware) {
			for (Found &y : g_found) if (y.kind == "firmware") y.pick = false;   /* (rows come sorted by version) */
			x.pick = true;
		}
		g_found.push_back(x);
	}
	fclose(f);
}
static bool found_ready(const Found &x) {   /* it has what adding it needs */
	if (x.kind == "firmware") return true;
	if (!kept_licence(x.content_id).empty()) return true;
	if (x.licence == "rif" || (x.kind == "game" && x.licence == "no")) return !x.licence_file.empty();
	if (x.kind == "update" && x.licence == "no") {   /* it decrypts with its game's licence: the game is being added with one */
		for (const Found &y : g_found) if (y.pick && y.kind == "game" && y.id == x.id && found_ready(y)) return true;
		return false;
	}
	return true;
}
static std::vector<Step> found_steps(int *n, std::vector<std::string> *titles) {   /* the firmware first, then per game: the game and its update kept, imported, built once */
	std::vector<Step> steps;   /* (each its own group: one failing doesn't stop the rest) */
	int group = 0;
	*n = 0;
	for (const Found &x : g_found)
		if (x.pick && x.kind == "firmware") { steps.push_back(step("Installing the firmware " + x.version, vitarecomp({"firmware", x.path}), ++group)); ++*n; break; }
	std::vector<std::string> ids;
	for (const Found &x : g_found) if (x.pick && (x.kind == "game" || x.kind == "update") && std::find(ids.begin(), ids.end(), x.id) == ids.end()) ids.push_back(x.id);
	if (titles) *titles = ids;
	for (const std::string &id : ids) {
		std::vector<const Found *> items;
		for (const char *k : {"game", "update"})
			for (const Found &x : g_found) if (x.pick && x.id == id && x.kind == k && found_ready(x)) { items.push_back(&x); break; }
		if (items.empty()) continue;
		bool pack = false;
		for (const Found &x : g_found) pack |= x.id == id && x.status != "nopack";
		++group;
		for (const Found *xp : items) {
			const Found &x = *xp;
			std::vector<std::string> argv = vitarecomp({"add", x.path});
			if (!x.licence_file.empty()) argv.insert(argv.end(), {"--license", x.licence_file});
			argv.push_back("--no-build");
			steps.push_back(step(x.kind == "update" ? "Adding its update " + x.version : "Adding " + x.name + " " + x.version, argv, group));
			++*n;
		}
		steps.push_back(step("Checking its version", vitarecomp({"import", id}), group));   /* (the game with its update layered over it) */
		if (pack) build_steps(steps, id, group);
	}
	return steps;
}
static void found_status(const Found &x) {   /* what its game pack says of it, as a dot and a line */
	struct S { const char *key, *text; int tone; };   /* tone: 0 good, 1 a caution, 2 neutral */
	static const S st[] = {
		{"recommended", "Recommended version", 0}, {"supported", "Supported version", 0}, {"check", "Checked against its game pack when added", 2},
		{"unsupported", "This version isn't supported yet", 1}, {"nopack", "No game pack yet: experimental", 1},
		{"otherregion", "This region's release isn't supported yet", 1},
		{"added", "Already in your library", 2}, {"licensed", "Nothing to add: the game gets its add-ons", 2},
		{"installed", "Already installed", 2}, {"firmware", "Not installed yet", 0}};
	for (const S &e : st) if (x.status == e.key) {
		ui::dot(e.tone == 0 ? ui::ready : e.tone == 1 ? ui::warn : ui::border);
		ImGui::TextDisabled("%s", e.text);
		if (!x.note.empty()) { ImGui::PushTextWrapPos(0.0f); ImGui::PushStyleColor(ImGuiCol_Text, ui::muted); ImGui::TextWrapped("%s.", x.note.c_str()); ImGui::PopStyleColor(); ImGui::PopTextWrapPos(); }
		return;
	}
	ui::dot(ui::border); ImGui::TextDisabled("%s", x.status.c_str());
}
static void found_list(void) {
	const float em = ImGui::GetFontSize();
	if (g_found.empty()) { ui::muted_wrapped("Nothing found there: no game or update folders (with sce_sys/param.sfo), PKGs or firmware update files (.PUP)."); return; }
	float row_h = ImGui::GetTextLineHeightWithSpacing() * 3.2f;
	ImGui::BeginChild("found", ImVec2(0, std::min(em * 17.0f, row_h * g_found.size() + em)), ImGuiChildFlags_Borders);
	for (size_t i = 0; i < g_found.size(); i++) {
		Found &x = g_found[i];
		ImGui::PushID((int)i);
		if (i) ImGui::Separator();
		bool can = x.kind == "game" || x.kind == "update" || (x.kind == "firmware" && x.status == "firmware");
		ImGui::BeginDisabled(!can || g_task.running);
		if (ImGui::Checkbox("##pick", &x.pick) && x.pick)   /* one copy of a game (or of its update) at a time */
			for (Found &y : g_found) if (&y != &x && y.kind == x.kind && y.id == x.id && x.kind != "firmware") y.pick = false;
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::BeginGroup();
		const char *kind = x.kind == "game" ? "Game" : x.kind == "update" ? "Update" : x.kind == "addon" ? "Add-on" : "Firmware file";
		ImGui::TextUnformatted(x.name.c_str()); ImGui::SameLine();
		ImGui::TextDisabled("%s%s%s%s", kind, x.version.empty() ? "" : " ", x.version.c_str(), x.form == "pkg" ? " (PKG)" : "");
		found_status(x);
		if (x.pick && (x.licence == "rif" || (x.kind == "game" && x.licence == "no")) && kept_licence(x.content_id).empty()) {
			ui::dot(ui::warn); ImGui::TextDisabled("Needs its licence file (.rif or work.bin).");
			licence_help_link();
			if (path_row("##licfile", x.licence_file, "Its licence file (.rif or work.bin)")) pick_file(nullptr, &x.licence_file, nullptr, 0);
		} else if (x.pick && x.kind == "update" && x.licence == "no" && !found_ready(x)) {
			ui::dot(ui::warn); ImGui::TextDisabled("Add its game too: an update decrypts with its game's licence");
		}
		ImGui::PushFont(nullptr, em * 0.85f); ImGui::PushStyleColor(ImGuiCol_Text, ui::muted);
		ImGui::TextUnformatted(x.path.c_str());
		if (!kept_licence(x.content_id).empty() && (x.kind == "game" || x.kind == "update")) ImGui::TextUnformatted("Licence: kept");
		else if (x.licence == "file") ImGui::Text("Licence: %s", x.licence_file.c_str());
		ImGui::PopStyleColor(); ImGui::PopFont();
		ImGui::EndGroup();
		ImGui::PopID();
	}
	ImGui::EndChild();
}
static bool search_form(SDL_Window *win) {   /* a folder searched for games, updates and the firmware file; true when adding started */
	ui::muted_wrapped("A folder to look through, every subfolder too: a copy of your Vita's ux0: (its app, patch and addcont folders) or a downloads folder. Games, their updates and the firmware file are found and checked against the game packs, which play the games' US releases at the versions listed.");
	ImGui::Spacing();
	if (path_row("##scanpath", g_scan_path, "Choose a folder to search")) pick_folder(win, &g_scan_path);
	const bool searching = g_task.running && g_task.kind == "scan";
	ImGui::BeginDisabled(g_scan_path.empty() || g_task.running);
	bool go = g_found_ready ? ImGui::Button("Search again") : ui::primary_button("Search");
	ImGui::EndDisabled();
	if (go) { g_found.clear(); g_found_ready = false; task_start("Searching a folder", "scan", "", {step("Searching", vitarecomp({"scan", g_scan_path, "--import-licences", "--out", scan_file()}))}); }
	if (searching) { ImGui::SameLine(); ui::spinner(ui::accent); ImGui::TextDisabled("Searching…"); }
	if (!g_found_ready) return false;
	ImGui::Spacing();
	found_list();
	int n = 0; std::vector<std::string> titles; std::vector<Step> steps = found_steps(&n, &titles);
	ImGui::Spacing();
	ImGui::BeginDisabled(n == 0 || g_task.running);
	std::string label = n ? "Add " + std::to_string(n) + (n == 1 ? " item" : " items") : "Add";
	bool started = ui::primary_button(label.c_str());
	ImGui::EndDisabled();
	int waiting = 0; for (const Found &x : g_found) waiting += x.pick && (x.kind == "game" || x.kind == "update") && !found_ready(x);
	if (waiting) { ImGui::SameLine(); ImGui::TextDisabled("%d waiting for a licence", waiting); }
	if (started) {
		task_start(n == 1 ? "Adding from the folder" : "Adding " + std::to_string(n) + " items", "add", "", steps);
		g_task.games = titles;   /* (their cards show the chain's step) */
		g_found.clear(); g_found_ready = false;
	}
	return started;
}
static bool add_form(SDL_Window *win) {   /* a player's own copy -> decrypted, imported, analysed, recompiled, built; true when started */
	static const char *const kinds[] = {"Search a folder", "A PKG file", "A game folder"};
	ui::segmented(kinds, 3, &g_add_kind); tab_hint();
	g_add_kind = (g_add_kind + tab_step() + 3) % 3;
	ImGui::Dummy(ImVec2(0, ImGui::GetFontSize() * 0.15f));
	if (g_add_kind == 0) return search_form(win);
	const bool pkg_kind = g_add_kind == 1;
	ui::muted_wrapped(pkg_kind ? "The .pkg of a game you own, with its licence file: the .rif from your Vita. Or its update's PKG, once the game is added (it uses the game's licence)."
	                           : "A game folder copied from your Vita (ux0:app/<title>: it carries its licence, sce_sys/package/work.bin), or one already decrypted; or its update (ux0:patch/<title>), once the game is added.");
	licence_help_link();
	ImGui::Spacing();
	if (path_row("##addpath", g_add_path, pkg_kind ? "Choose the .pkg file" : "Choose the game folder")) {
		static const SDL_DialogFileFilter pkg[] = {{"PS Vita package", "pkg"}};
		if (pkg_kind) pick_file(win, &g_add_path, pkg, 1); else pick_folder(win, &g_add_path);
	}
	if (path_row("##lic", g_add_license, pkg_kind ? "Its licence file (.rif or work.bin)" : "Its licence file, if the folder has none (.rif or work.bin)")) pick_file(win, &g_add_license, nullptr, 0);
	ImGui::Spacing();
	bool started = false;
	ImGui::BeginDisabled(g_add_path.empty() || g_task.running);
	if (ui::primary_button("Add this game")) {
		std::vector<std::string> argv = vitarecomp({"add", g_add_path});
		if (!g_add_license.empty()) argv.insert(argv.end(), {"--license", g_add_license});
		task_start("Adding a game", "add", "", {step("Decrypting and importing your copy", argv), step("Added")});
		g_add_path.clear(); g_add_license.clear();
		started = true;
	}
	ImGui::EndDisabled();
	return started;
}
static void task_line(void) {   /* the running task, inline: spinner, its step, Cancel; or the last failure */
	if (g_task.running) {
		ui::spinner(ui::accent);
		ImGui::TextUnformatted(g_task.name.c_str()); ImGui::SameLine(); ImGui::TextDisabled("%s", task_status().c_str());
	} else if (!g_failed.empty()) {
		ui::dot(ui::error); ImGui::TextColored(ui::error, "%s didn’t finish.", g_failed.c_str()); ImGui::SameLine();
		if (ImGui::TextLink("Show the log")) g_drawer = true;
	}
}

/* ---- settings (runtime/core/settings.h): the shared file every game reads -- display, controls, sound, online ID -- beside this
 * launcher's own files, and each game's own (its options, and where it keeps a value of its own). A game reads its own
 * file first, then the shared one; a change in any game's menu changes the shared file. The launcher's Settings page
 * edits the shared file, a card's Game settings its game's. Each change is read-modify-written at once (a running game
 * merges its own changes the same way). */
struct Ini { std::vector<std::pair<std::string, std::string>> kv; };
static Ini ini_load(const std::string &path) {
	Ini x; FILE *f = fopen(path.c_str(), "r"); if (!f) return x;
	char line[512];
	while (fgets(line, sizeof line, f)) {
		std::string l = line; size_t eq = l.find('=');
		size_t b = l.find_first_not_of(" \t"); if (b == std::string::npos || l[b] == '#' || l[b] == ';' || eq == std::string::npos) continue;
		auto trim = [](std::string v) { size_t a = v.find_first_not_of(" \t\r\n"), e = v.find_last_not_of(" \t\r\n"); return a == std::string::npos ? std::string() : v.substr(a, e - a + 1); };
		x.kv.push_back({trim(l.substr(0, eq)), trim(l.substr(eq + 1))});
	}
	fclose(f);
	return x;
}
static const std::string *ini_find(const Ini &x, const std::string &k) { for (auto &e : x.kv) if (e.first == k) return &e.second; return nullptr; }
static void ini_set(Ini &x, const std::string &k, const std::string &v) { for (auto &e : x.kv) if (e.first == k) { e.second = v; return; } x.kv.push_back({k, v}); }
static void ini_erase(Ini &x, const std::string &k) { x.kv.erase(std::remove_if(x.kv.begin(), x.kv.end(), [&](const std::pair<std::string, std::string> &e) { return e.first == k; }), x.kv.end()); }
static void ini_save(const Ini &x, const std::string &path, const char *header) {
	std::string tmp = path + ".tmp"; FILE *f = fopen(tmp.c_str(), "w"); if (!f) return;
	fprintf(f, "%s\n", header);
	for (auto &e : x.kv) fprintf(f, "%s = %s\n", e.first.c_str(), e.second.c_str());
	if (fclose(f) == 0) {
#ifdef _WIN32
		MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);   /* (rename refuses an existing file on Windows) */
#else
		rename(tmp.c_str(), path.c_str());
#endif
	}
}
static const char *const SHARED_HEADER = "# Project Nativ's settings for every game (the launcher's Settings, and each game's menu; runtime/core/settings.h)";
static const char *const GAME_HEADER = "# vitarecomp settings for this game: its own options, and where it differs from the shared settings (runtime/core/settings.h)";
static bool game_key(const std::string &k) {   /* (settings.c's game_key: kept in the game's own file) */
	for (const char *g : {"fov", "view_distance", "scenery_detail", "character_detail", "private_server", "net_redirect", "menu_hint_shown"}) if (k == g) return true;
	return false;
}
/* the shared keys' defaults where a game reads no value (runtime/core/settings.h; the consumers' own) */
static const char *setting_default(const std::string &k) {
	static const std::pair<const char *, const char *> d[] = {{"render_scale", "1"}, {"fullscreen", "0"}, {"aspect_ratio", "native"}, {"hud_layout", "edges"},
		{"aspect", "fit"}, {"frame_rate", "30"}, {"post_aa", "1"}, {"sharpen", "0.35"}, {"anisotropy", "16"}, {"shadow_filter", "1"}, {"full_res_world", "1"},
		{"ssao", "1"}, {"ssao_strength", "1"}, {"osd", "0"}, {"input_mode", "auto"}, {"mouse_look", "1"}, {"raw_look", "1"}, {"mouse_sens", "1"},
		{"mouse_invert", "0"}, {"touch_cursor_speed", "1"}, {"gyro", "1"}, {"gyro_pose", "upright"}, {"gyro_sens", "1"}, {"volume", "1"}, {"online_id", ""}, {"online", "1"},
		{"camera", "light"}, {"save_dialog", "ask"}, {"audio_sync_trim_ms", "0"}, {"audio_buffers", "1"}};
	for (auto &e : d) if (k == e.first) return e.second;
	return nullptr;
}
/* keys only some games read (an online one, a camera one, one with ambient occlusion): a game without the key never chose */
static bool some_games_key(const std::string &k) { return k == "online_id" || k == "online" || k == "np_account_id" || k == "np_age" || k == "camera" || k == "ssao" || k == "ssao_strength"; }
static std::string shared_path(void) { std::string d = pref_dir(); return d.empty() ? "" : d + "settings.ini"; }
static std::string game_settings_path(const std::string &id) { return g_dump + "/decrypted/" + id + "/recomp/settings.ini"; }
static std::string fmt_g(double v) { char b[32]; snprintf(b, sizeof b, "%g", v); return b; }

/* the starting point on a new machine: the look most players want from a PC version -- fullscreen, sharp for this display,
 * the full width of the screen, 60 fps, the mouse aiming 1:1, softer shadows (the games' own menus, and Settings, change it) */
static int recommended_scale(void) {
	const SDL_DisplayMode *m = SDL_GetCurrentDisplayMode(SDL_GetPrimaryDisplay());
	float h = m ? m->h * (m->pixel_density > 0 ? m->pixel_density : 1.0f) : 1080;
	int k = (int)lroundf(h / 544.0f); return k < 2 ? 2 : k > 4 ? 4 : k;
}
static void recommended_into(Ini &x) {
	ini_set(x, "fullscreen", "1"); ini_set(x, "render_scale", std::to_string(recommended_scale())); ini_set(x, "aspect_ratio", "window");
	ini_set(x, "frame_rate", "60"); ini_set(x, "raw_look", "1"); ini_set(x, "shadow_filter", "2");
}
/* once, when there is no shared file yet: it starts as what this library's games agree on (each key: the value most of
 * their files hold; a tie, the most recently changed file's), or the recommended set on a new install. Then, every
 * start, a game's file drops a shared key holding the same value as the shared file -- no change in what it plays with,
 * and what's left in it is what differs (its Game settings list those) */
static void settings_migrate(void) {
	std::string sp = shared_path(); if (sp.empty()) return;
	struct F { std::string path; Ini x; time_t mtime; };
	std::vector<F> files;
	if (DIR *d = opendir((g_dump + "/decrypted").c_str())) {
		while (dirent *e = readdir(d)) {
			if (e->d_name[0] == '.') continue;
			std::string p = g_dump + "/decrypted/" + e->d_name + "/recomp/settings.ini"; struct stat st;
			if (stat(p.c_str(), &st) == 0) files.push_back({p, ini_load(p), st.st_mtime});
		}
		closedir(d);
	}
	/* (a shared file holding only np_* keys is a game's account record -- a game ran before the launcher ever did -- not
	 * anyone's choices: it starts like a new one, those keys kept) */
	Ini before = exists(sp) ? ini_load(sp) : Ini();
	bool chosen = false; for (auto &e : before.kv) chosen |= e.first.rfind("np_", 0) != 0;
	if (!chosen) {
		/* a game whose file lacks a key plays with its default: a vote for leaving the key out (but for keys only some
		 * games read) -- so one game's choice stays that game's, and what most games chose becomes everyone's */
		Ini sh; std::map<std::string, std::map<std::string, std::pair<int, time_t>>> votes;
		const std::string ABSENT = std::string("\x01") + "absent";
		for (const F &f : files) for (auto &e : f.x.kv) if (!game_key(e.first) && e.first.rfind("bind_", 0) != 0) {
			auto &v = votes[e.first][e.second]; v.first++; if (f.mtime > v.second) v.second = f.mtime;
		}
		for (auto &k : votes) if (!some_games_key(k.first))
			for (const F &f : files) if (!ini_find(f.x, k.first)) { auto &v = k.second[ABSENT]; v.first++; if (f.mtime > v.second) v.second = f.mtime; }
		for (auto &k : votes) {
			const std::string *best = nullptr; std::pair<int, time_t> bv{0, 0};
			for (auto &v : k.second) if (v.second > bv) { bv = v.second; best = &v.first; }
			if (best && *best != ABSENT) ini_set(sh, k.first, *best);
		}
		bool voted = false; for (auto &k : votes) voted |= k.first.rfind("np_", 0) != 0;   /* (account keys alone: no one's choices) */
		if (!voted) recommended_into(sh);
		for (auto &e : before.kv) ini_set(sh, e.first, e.second);
		ini_save(sh, sp, SHARED_HEADER);
		fprintf(stderr, "settings: the shared file started %s (%s)\n", !voted ? "with the recommended set" : "from this library's games", sp.c_str());
	}
	Ini sh = ini_load(sp);
	for (F &f : files) {
		size_t before = f.x.kv.size();
		f.x.kv.erase(std::remove_if(f.x.kv.begin(), f.x.kv.end(), [&](const std::pair<std::string, std::string> &e) {
			if (game_key(e.first)) return false;
			const std::string *v = ini_find(sh, e.first); const char *d = setting_default(e.first);   /* (the value it would read without it) */
			return v ? *v == e.second : d && atof(d) == atof(e.second.c_str()) && (isdigit((unsigned char)d[0]) || e.second == d); }), f.x.kv.end());
		if (f.x.kv.size() != before) ini_save(f.x, f.path, GAME_HEADER);
	}
}

/* the Settings page's controls: each a settings row (ui_theme.h) over one key of an Ini, written at once */
struct SetTarget { Ini *x; std::string path; const char *header; };
static void target_write(const SetTarget &t, const std::string &k, const std::string &v) {   /* read-modify-write: a running game's changes stay */
	ini_set(*t.x, k, v);
	Ini disk = ini_load(t.path); ini_set(disk, k, v); ini_save(disk, t.path, t.header);
}
static std::string tget(const SetTarget &t, const char *k, const char *def) { const std::string *v = ini_find(*t.x, k); return v ? *v : def; }
static void seg_row(const SetTarget &t, const char *label, const char *expl, const char *key, const char *def, std::initializer_list<std::pair<const char *, const char *>> opts) {
	ui::row(label, expl);
	ImGui::PushID(key);
	std::vector<const char *> names; std::string cur = tget(t, key, def); int v = 0, i = 0;
	for (auto &o : opts) { if (cur == o.first) v = i; names.push_back(o.second); i++; }
	if (ui::segmented(names.data(), (int)names.size(), &v)) target_write(t, key, opts.begin()[v].first);
	ImGui::PopID();
}
static void toggle_row(const SetTarget &t, const char *label, const char *expl, const char *key, bool def) {
	ui::row(label, expl);
	bool on = atoi(tget(t, key, def ? "1" : "0").c_str()) != 0;
	if (ui::toggle(key, &on)) target_write(t, key, on ? "1" : "0");
}
static void slider_row(const SetTarget &t, const char *label, const char *expl, const char *key, double def, float lo, float hi, const char *fmt, float shown = 1, ImGuiSliderFlags fl = 0) {
	ui::row(label, expl);
	float v = (float)atof(tget(t, key, fmt_g(def).c_str()).c_str()) * shown;
	ImGui::SetNextItemWidth(-FLT_MIN);
	std::string id = std::string("##") + key;
	if (ImGui::SliderFloat(id.c_str(), &v, lo, hi, fmt, fl)) ini_set(*t.x, key, fmt_g(v / shown));
	if (ImGui::IsItemDeactivatedAfterEdit()) target_write(t, key, tget(t, key, ""));   /* (written once the drag ends) */
}
static bool g_settings_open; static int g_settings_tab; static Ini g_shared;
static bool g_setup_online = true;   /* setup's Play online, written with the name */
static bool g_settings_writable = true;   /* (false in a screenshot run without ATIV_PREFS: it touches no one's settings) */

/* the online name (runtime/net/np.c np_online_id makes the same): others see "ntv_" and the player's name, up to 10
 * letters, digits, '-' or '_' -- or, with none chosen, "ntv_" and a tag of six from this install's account ID (settings
 * np_account_id: random, made once, here when no game has made it yet) */
/* online_status.txt beside the shared settings (runtime/net/online/status.c): a game playing online rewrites it every 5 s.
 * "" when there is none */
static std::string online_status_line(void) {
	std::string d = pref_dir(); if (d.empty()) return "";
	Ini st = ini_load(d + "online_status.txt");
	const std::string *tm = ini_find(st, "time"); if (!tm) return "";
	const long long ago = (long long)time(nullptr) - atoll(tm->c_str());
	auto num = [&](const char *k) { const std::string *v = ini_find(st, k); return v ? atoi(v->c_str()) : 0; };
	const std::string *g = ini_find(st, "game"); const std::string game = g && !g->empty() ? *g : "a game";
	const int linked = num("linked"), rooms = num("rooms_heard") + num("rooms_mine");
	char b[256];
	if (ago >= 0 && ago < 15) snprintf(b, sizeof b, "Online now in %s: %s, %d %s open.", game.c_str(), linked ? (std::to_string(linked) + (linked == 1 ? " other copy linked" : " other copies linked")).c_str() : "looking for other players", rooms, rooms == 1 ? "room" : "rooms");
	else {
		const long long m = ago / 60;
		const std::string when = m < 1 ? "a moment ago" : m < 60 ? std::to_string(m) + " min ago" : m < 48 * 60 ? std::to_string(m / 60) + " h ago" : std::to_string(m / 1440) + " days ago";
		snprintf(b, sizeof b, "Last online %s, in %s%s.", when.c_str(), game.c_str(), linked ? (", linked with " + std::to_string(linked) + (linked == 1 ? " other copy" : " other copies")).c_str() : "");
	}
	return b;
}
static const char *ONLINE_EXPL = "Games' online modes (Killzone: Mercenary, Unit 13) with other Project Nativ players, on your network and over the internet. They see your online name and your computer's IP address, as in any peer-to-peer game.";
static bool online_name_ok(const std::string &n) {
	return n.size() <= 10 && std::all_of(n.begin(), n.end(), [](char c) { return isalnum((unsigned char)c) || c == '-' || c == '_'; });
}
static std::string online_name(const Ini &shared) {   /* the chosen name, without a tag a user may have typed: "ntv_", or the ones before, "nativ_" and Project Ativ's "ativ_" */
	const std::string *v = ini_find(shared, "online_id"); std::string n = v ? *v : "";
	for (const char *tag : {"ntv_", "nativ_", "ativ_"}) if (!strncasecmp(n.c_str(), tag, strlen(tag))) return n.substr(strlen(tag));
	return n;
}
static bool game_option(const Game &g, const char *name);
static bool needs_online_name(const Game &g) {   /* asked once: an online game, online on, and the player hasn't named themselves */
	if (g_prefs["online_name_asked"] == "1" || !game_option(g, "online")) return false;
	const Ini sh = ini_load(shared_path());
	const std::string *on = ini_find(sh, "online");
	return !(on && atoi(on->c_str()) == 0) && online_name(sh).empty();
}
static std::string online_tag(Ini &shared) {
	const std::string *a = ini_find(shared, "np_account_id");
	uint64_t v = a ? strtoull(a->c_str(), nullptr, 16) : 0;
	if (!v) {
		std::random_device rd; v = ((uint64_t)rd() << 32 | rd()) | 1;
		char b[24]; snprintf(b, sizeof b, "%016llx", (unsigned long long)v);
		if (g_settings_writable) target_write(SetTarget{&shared, shared_path(), SHARED_HEADER}, "np_account_id", b);
	}
	static const char al[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";   /* (no 0/O, 1/I lookalikes) */
	std::string tag; for (int n = 0; n < 6; n++, v >>= 5) tag += al[v & 31];
	return tag;
}
static void settings_modal(void) {
	const ImGuiViewport *vp = ImGui::GetMainViewport();
	const float em = ImGui::GetFontSize();
	if (g_settings_open && !ImGui::IsPopupOpen("Settings")) { ImGui::OpenPopup("Settings"); g_shared = ini_load(shared_path()); }
	ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x < 50 * em ? vp->WorkSize.x * 0.94f : 46 * em, vp->WorkSize.y * 0.86f), ImGuiCond_Always);
	ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(1.6f * em, 1.3f * em)); ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
	if (!ImGui::BeginPopupModal("Settings", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove)) { ImGui::PopStyleVar(2); return; }
	ImGui::PopStyleVar(2);
	ui::heading("Settings", em * 1.4f);
	ui::muted_wrapped("For every game. A game reads them when it starts; its own menu (F10) changes them too, for every game. A game's own options, and any value it keeps for itself, are under Game settings on its card.");
	ImGui::Dummy(ImVec2(0, em * 0.2f));
	static const char *const tabs[] = {"Display##st", "Controls##st", "Sound & online##st"};
	ui::segmented(tabs, 3, &g_settings_tab); tab_hint();
	g_settings_tab = (g_settings_tab + tab_step() + 3) % 3;
	ImGui::Dummy(ImVec2(0, em * 0.2f));
	SetTarget t{&g_shared, shared_path(), SHARED_HEADER};
	ImGui::BeginChild("settings_rows", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() - em * 0.4f), 0);
	if (ui::rows_begin("shared")) {
		if (g_settings_tab == 0) {
			ui::row("Resolution", "The game drawn at a multiple of the Vita's 960 × 544. Higher is sharper; 4× and up want a fast GPU.");
			{	ImGui::PushID("render_scale");
				const char *const names[6] = {"1×", "2×", "3×", "4×", "5×", "6×"}; int v = atoi(tget(t, "render_scale", "1").c_str()) - 1; if (v < 0) v = 0; if (v > 5) v = 5;
				if (ui::segmented(names, 6, &v)) target_write(t, "render_scale", std::to_string(v + 1));
				int r = recommended_scale(); if (r != v + 1) { char l[48]; snprintf(l, sizeof l, "%d× suits this display", r); if (ImGui::TextLink(l)) target_write(t, "render_scale", std::to_string(r)); }
				ImGui::PopID(); }
			toggle_row(t, "Fullscreen", "Also kept when you use the window's own fullscreen button.", "fullscreen", false);
			seg_row(t, "Widescreen", "Shows more of the game at the sides, in games that support it.", "aspect_ratio", "native", {{"native", "Off"}, {"21:9", "21:9"}, {"32:9", "32:9"}, {"window", "Window"}});
			seg_row(t, "HUD", "With widescreen on: the HUD at the screen's edges, in a 16:9 frame in the middle, or stretched.", "hud_layout", "edges", {{"edges", "Edges"}, {"16:9", "16:9"}, {"stretch", "Stretched"}});
			seg_row(t, "Picture fit", "How the picture fills the window.", "aspect", "fit", {{"fit", "Fit"}, {"stretch", "Stretch"}, {"fill", "Fill"}});
			seg_row(t, "Frame rate", "60 is experimental: parts of a game timed in fixed 1/30 s steps can run fast.", "frame_rate", "30", {{"30", "30"}, {"60", "60"}});
			toggle_row(t, "Anti-aliasing", "Smooths jagged edges (FXAA).", "post_aa", true);
			slider_row(t, "Sharpening", "Brings back detail lost to scaling and anti-aliasing.", "sharpen", 0.35, 0.0f, 100.0f, "%.0f%%", 100.0f);
			seg_row(t, "Texture filtering", "Keeps textures crisp when seen at an angle.", "anisotropy", "16", {{"1", "Off"}, {"2", "2×"}, {"4", "4×"}, {"8", "8×"}, {"16", "16×"}});
			seg_row(t, "Shadow edges", "How soft the edges of shadows are.", "shadow_filter", "1", {{"0", "Original"}, {"1", "Soft"}, {"2", "Softer"}});
			toggle_row(t, "Full-resolution world", "Games that draw their 3D world below the screen's resolution draw it at the full one. Sharper, at a GPU cost.", "full_res_world", true);
			toggle_row(t, "Ambient occlusion", "Soft shadows where surfaces meet, in games that support it.", "ssao", true);
			slider_row(t, "Occlusion strength", nullptr, "ssao_strength", 1.0, 0.0f, 200.0f, "%.0f%%", 100.0f);
			seg_row(t, "On-screen stats", "Frame rate and timings over the game. F1 cycles them.", "osd", "0", {{"0", "Off"}, {"1", "FPS"}, {"2", "Full"}});
		} else if (g_settings_tab == 1) {
			seg_row(t, "Play with", "Automatic follows the last device you touch.", "input_mode", "auto", {{"auto", "Automatic"}, {"pad", "Controller"}, {"kbm", "Keyboard & mouse"}});
			toggle_row(t, "Mouse look", "The mouse turns the camera and aims; left button R, right button L.", "mouse_look", true);
			seg_row(t, "Mouse aim", "Direct: the game turns exactly as far as the mouse moves (Uncharted, Unit 13, Killzone). Stick: the mouse moves the right stick. F8 switches in a game.", "raw_look", "1", {{"1", "Direct"}, {"0", "Stick"}});
			slider_row(t, "Mouse sensitivity", nullptr, "mouse_sens", 1.0, 0.1f, 5.0f, "%.2f×", 1, ImGuiSliderFlags_Logarithmic);
			toggle_row(t, "Invert vertical look", nullptr, "mouse_invert", false);
			slider_row(t, "Touch cursor speed", "Hold a bumper and move a stick: the cursor is a finger on the touch screen.", "touch_cursor_speed", 1.0, 0.25f, 3.0f, "%.2f×");
			toggle_row(t, "Gyro", "A controller's gyro as the Vita's motion sensor.", "gyro", true);
			seg_row(t, "A level controller is a Vita held", "Upright, as in your hands, or flat on a table.", "gyro_pose", "upright", {{"upright", "Upright"}, {"flat", "Flat"}});
			slider_row(t, "Gyro sensitivity", nullptr, "gyro_sens", 1.0, 0.25f, 3.0f, "%.2f×");
		} else {
			slider_row(t, "Volume", nullptr, "volume", 1.0, 0.0f, 100.0f, "%.0f%%", 100.0f);
			toggle_row(t, "Play online", ONLINE_EXPL, "online", true);
			{ const std::string st = online_status_line(); if (!st.empty()) ui::muted_wrapped(st.c_str()); }
			ui::row("Online name", "Others see ntv_ and your name, so players know you're on PC: up to 10 letters, digits, '-' or '_'.");
			{	static std::string oid; static bool init; if (!init || !ImGui::IsPopupOpen("Settings")) { oid = online_name(*t.x); init = true; }
				ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("ntv_"); ImGui::SameLine(0, 0);
				input_text("##oid", oid, -FLT_MIN);
				const bool ok = online_name_ok(oid);
				if (ImGui::IsItemDeactivatedAfterEdit() && ok) target_write(t, "online_id", oid);
				if (!ok) ui::muted_wrapped("Not saved: up to 10 letters, digits, '-' or '_'");
				else if (oid.empty()) ui::muted_wrapped(("Empty: you play as ntv_" + online_tag(*t.x) + ", a tag made for this computer.").c_str());
			}
		}
		ui::rows_end();
	}
	ImGui::EndChild();
	if (ImGui::Button("Use the recommended set")) { Ini r; recommended_into(r); for (auto &e : r.kv) target_write(t, e.first, e.second); }
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("Fullscreen, %d× resolution, widescreen to the window, 60 fps, direct mouse aim, softer shadows", recommended_scale());
	ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::CalcTextSize("Done").x - ImGui::GetStyle().FramePadding.x * 2);
	if (ui::primary_button("Done")) { g_settings_open = false; ImGui::CloseCurrentPopup(); }
	ImGui::EndPopup();
}

/* a game's own settings: its options (the pack's RECOMP_GAME_OPTIONS -- the ones its hooks honour) and the values it keeps
 * apart from the shared ones, each of which can go back to the shared value */
static std::string g_game_settings;   /* the game whose dialog is open ("": none) */
static Ini g_game_ini;
static bool game_option(const Game &g, const char *name) {
	std::string env = pack_value(pack_read(g_root + "/packs/" + g.id + "/pack.toml"), "RUN_ENV") + " ";
	size_t a = env.find("RECOMP_GAME_OPTIONS="); if (a == std::string::npos) return false;
	std::string list = "," + env.substr(a + 20, env.find(' ', a) - a - 20) + ",";
	return list.find(std::string(",") + name + ",") != std::string::npos;
}
static const char *setting_name(const std::string &k) {
	static const std::pair<const char *, const char *> names[] = {{"render_scale", "Resolution"}, {"fullscreen", "Fullscreen"}, {"aspect_ratio", "Widescreen"},
		{"hud_layout", "HUD"}, {"aspect", "Picture fit"}, {"frame_rate", "Frame rate"}, {"post_aa", "Anti-aliasing"}, {"sharpen", "Sharpening"},
		{"anisotropy", "Texture filtering"}, {"shadow_filter", "Shadow edges"}, {"full_res_world", "Full-resolution world"}, {"ssao", "Ambient occlusion"},
		{"ssao_strength", "Occlusion strength"}, {"osd", "On-screen stats"}, {"input_mode", "Play with"}, {"mouse_look", "Mouse look"}, {"raw_look", "Mouse aim"},
		{"mouse_sens", "Mouse sensitivity"}, {"mouse_invert", "Invert vertical look"}, {"volume", "Volume"}, {"online_id", "Online name"}, {"online", "Play online"}, {"gyro", "Gyro"},
		{"gyro_sens", "Gyro sensitivity"}, {"camera", "Rear camera"}};
	for (auto &n : names) if (k == n.first) return n.second;
	return nullptr;
}
static void game_settings_modal(void) {
	const ImGuiViewport *vp = ImGui::GetMainViewport();
	const float em = ImGui::GetFontSize();
	const Game *g = nullptr; for (const Game &x : g_games) if (x.id == g_game_settings) g = &x;
	if (!g_game_settings.empty() && !ImGui::IsPopupOpen("Game settings")) { ImGui::OpenPopup("Game settings"); g_game_ini = ini_load(game_settings_path(g_game_settings)); }
	ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x < 44 * em ? vp->WorkSize.x * 0.94f : 40 * em, 0), ImGuiCond_Always);
	ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(1.6f * em, 1.3f * em)); ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
	if (!ImGui::BeginPopupModal("Game settings", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove)) { ImGui::PopStyleVar(2); return; }
	ImGui::PopStyleVar(2);
	if (!g) { g_game_settings.clear(); ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return; }
	ui::heading((g->name + " settings").c_str(), em * 1.4f);
	SetTarget t{&g_game_ini, game_settings_path(g->id), GAME_HEADER};
	bool any = false;
	if (ui::rows_begin("game_own")) {
		if (game_option(*g, "fov")) { any = true; slider_row(t, "Field of view", "A wider view. 1.00× is the game's own.", "fov", 1.0, 0.75f, 1.5f, "%.2f×"); }
		if (game_option(*g, "view_distance")) { any = true; slider_row(t, "View distance", "How far fading scenery reaches. 1.00× is the game's own.", "view_distance", 1.0, 1.0f, 3.0f, "%.2f×"); }
		if (game_option(*g, "scenery_detail")) { any = true; slider_row(t, "Scenery detail", "How far small scenery details stay visible. 1.00× is the game's own.", "scenery_detail", 1.0, 1.0f, 8.0f, "%.1f×"); }
		if (game_option(*g, "character_detail")) { any = true; slider_row(t, "Character detail", "How far characters and props keep their full detail. 1.00× is the game's own.", "character_detail", 1.0, 1.0f, 8.0f, "%.1f×"); }
		if (game_option(*g, "private_server")) {   /* (a game whose online needs its makers' servers: Killzone; Unit 13 needs none) */
			any = true;
			ui::row("Private server", "The community server that stands in for the game's own (host, or host:port). Empty: the game's own, long offline.");
			static std::string srv; static std::string for_id; if (for_id != g->id) { srv = tget(t, "private_server", ""); for_id = g->id; }
			input_text("##srv", srv, -FLT_MIN);
			if (ImGui::IsItemDeactivatedAfterEdit()) target_write(t, "private_server", srv);
		}
		ui::rows_end();
	}
	if (!any) ui::muted_wrapped("This game has no options of its own yet.");
	/* what it keeps apart from the shared settings */
	Ini sh = ini_load(shared_path());
	std::vector<std::pair<std::string, std::string>> own;
	for (auto &e : g_game_ini.kv) if (!game_key(e.first) && setting_name(e.first)) own.push_back(e);
	ImGui::Dummy(ImVec2(0, em * 0.5f));
	if (own.empty()) ui::muted_wrapped("Everything else follows the shared Settings.");
	else {
		ImGui::TextUnformatted("Kept for this game only");
		ui::muted_wrapped("Values this game uses instead of the shared Settings (from before settings were shared, or set here).");
		for (auto &e : own) {
			ImGui::PushID(e.first.c_str());
			const std::string *sv = ini_find(sh, e.first);
			ImGui::Bullet(); ImGui::Text("%s: %s", setting_name(e.first), e.second.c_str());
			ImGui::SameLine(); ImGui::TextDisabled("(shared: %s)", sv ? sv->c_str() : setting_default(e.first) ? setting_default(e.first) : "default");
			ImGui::SameLine(); if (ImGui::SmallButton("Use shared")) { ini_erase(g_game_ini, e.first); Ini disk = ini_load(t.path); ini_erase(disk, e.first); ini_save(disk, t.path, GAME_HEADER); }
			ImGui::PopID();
		}
	}
	ImGui::Dummy(ImVec2(0, em * 0.4f));
	if (ui::primary_button("Done")) { g_game_settings.clear(); ImGui::CloseCurrentPopup(); }
	ImGui::EndPopup();
}

/* ---- Windows' Smart App Control. When it's on, a program that isn't signed and that Windows doesn't already know gets a
 * verdict from Microsoft's cloud, one per file (by its hash), and the verdict sticks. Every game Nativ builds is new to it
 * (made on this PC, from the player's own copy), and now and then one is refused: on one PC (2026-10-26) Shovel Knight's
 * build was, while Killzone's, the launcher, and the same Shovel Knight code with one byte more all started. A rebuild is
 * a new file (the linker stamps the time), checked afresh; turning Smart App Control off is the way left after that, as
 * Windows has no exception for one program. The dialog opens when Windows refused a game: vitarecomp's SAC_BLOCKED line */
static bool g_sac_open, g_sac_rebuilt;   /* (rebuilt: the player already rebuilt it once and it was refused again) */
static std::string g_sac_game;            /* the game Windows refused */
/* ---- the online name, before the first online game: what other players will see (the same field as Settings' Online name
 * and setup's). Asked once; Play with it empty keeps the tag made for this computer */
static void online_name_modal(void) {
	const ImGuiViewport *vp = ImGui::GetMainViewport();
	const float em = ImGui::GetFontSize();
	static std::string name;
	if (!g_name_game.empty() && !ImGui::IsPopupOpen("Online name")) { name.clear(); ImGui::OpenPopup("Online name"); }
	ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x < 40 * em ? vp->WorkSize.x * 0.94f : 32 * em, 0), ImGuiCond_Always);
	ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(1.6f * em, 1.3f * em)); ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
	if (!ImGui::BeginPopupModal("Online name", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove)) { ImGui::PopStyleVar(2); return; }
	ImGui::PopStyleVar(2);
	const Game *g = nullptr; for (const Game &x : g_games) if (x.id == g_name_game) g = &x;
	if (!g) { g_name_game.clear(); ImGui::CloseCurrentPopup(); ImGui::EndPopup(); return; }
	ui::heading("Your online name", em * 1.4f);
	ImGui::PushTextWrapPos(0);
	ImGui::TextUnformatted((g->name + " plays online with other Project Nativ players. Choose the name they'll see: it shows as ntv_ and your name.").c_str());
	ImGui::Dummy(ImVec2(0, em * 0.3f));
	ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("ntv_"); ImGui::SameLine(0, 0);
	if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
	input_text("##name_oid", name, em * 12, "your name");
	const bool ok = online_name_ok(name);
	if (!ok) ui::muted_wrapped("Up to 10 letters, digits, '-' or '_'.");
	else if (name.empty()) ui::muted_wrapped(("Left empty, you play as ntv_" + online_tag(g_shared) + ", a tag made for this computer.").c_str());
	ui::muted_wrapped("You can change it any time in Settings.");
	ImGui::PopTextWrapPos();
	ImGui::Dummy(ImVec2(0, em * 0.4f));
	ImGui::BeginDisabled(!ok);
	if (ui::primary_button("Play") || (ok && ImGui::IsKeyPressed(ImGuiKey_Enter))) {
		if (!name.empty() && g_settings_writable) target_write(SetTarget{&g_shared, shared_path(), SHARED_HEADER}, "online_id", name);
		prefs_set("online_name_asked", "1");
		const Game game = *g; g_name_game.clear(); ImGui::CloseCurrentPopup(); ImGui::EndDisabled(); ImGui::EndPopup();
		start_play_now(game);
		return;
	}
	ImGui::EndDisabled();
	ImGui::SameLine(); if (ImGui::Button("Not now") || ImGui::IsKeyPressed(ImGuiKey_Escape)) { g_name_game.clear(); ImGui::CloseCurrentPopup(); }
	ImGui::EndPopup();
}
static void sac_modal(void) {
	const ImGuiViewport *vp = ImGui::GetMainViewport();
	const float em = ImGui::GetFontSize();
	if (g_sac_open && !ImGui::IsPopupOpen("Smart App Control")) ImGui::OpenPopup("Smart App Control");
	ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x < 44 * em ? vp->WorkSize.x * 0.94f : 36 * em, 0), ImGuiCond_Always);
	ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(1.6f * em, 1.3f * em)); ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
	if (!ImGui::BeginPopupModal("Smart App Control", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove)) { ImGui::PopStyleVar(2); return; }
	ImGui::PopStyleVar(2);
	const Game *g = nullptr; for (const Game &x : g_games) if (x.id == g_sac_game) g = &x;
	ui::heading(("Windows blocked " + (g ? g->name : g_sac_game)).c_str(), em * 1.4f);
	ImGui::PushTextWrapPos(0);
	ImGui::TextUnformatted(g_sac_rebuilt ? "Windows' Smart App Control refused the rebuilt game too." : "Windows' Smart App Control refused to start it.");
	ImGui::Dummy(ImVec2(0, em * 0.3f));
	ImGui::TextUnformatted("Smart App Control checks each program Windows doesn't know with Microsoft's cloud, and every game Nativ builds is "
	                       "new to it, because it's built on this PC from your own copy. Its verdict is made once for each file, and now and "
	                       "then it refuses one. A rebuild makes a new file that Windows checks afresh, and that usually starts.");
	ImGui::Dummy(ImVec2(0, em * 0.3f));
	ImGui::TextUnformatted("If it's refused again, turn Smart App Control off: in Windows Security, open App & browser control, then Smart App "
	                       "Control settings, and choose Off. Windows has no way to allow a single program.");
	ImGui::Dummy(ImVec2(0, em * 0.2f));
	ui::muted_wrapped("On most versions of Windows 11, Smart App Control can't be turned back on without reinstalling Windows.");
	ImGui::PopTextWrapPos();
	ImGui::Dummy(ImVec2(0, em * 0.5f));
	auto rebuild = [&]() {   /* (a relink: the same code as a new file, then the game, as Play does) */
		Step b = step("Rebuilding " + g->name + " as a new file for Windows to check", vitarecomp({"build", g->id}));
		Step r = step("Running " + g->name, vitarecomp({"run", g->id})); r.env.push_back("ATIV_LAUNCHER=1");
		task_start("Playing " + g->name, "play", g->id, {b, r});
		g_sac_rebuilt = true; g_sac_open = false; ImGui::CloseCurrentPopup();
	};
	auto settings = [&]() { SDL_OpenURL("windowsdefender://smartapp"); };
	ImGui::BeginDisabled(!g || g_task.running);
	if (g_sac_rebuilt ? ImGui::Button("Rebuild and try again") : ui::primary_button("Rebuild and try again")) rebuild();
	ImGui::EndDisabled();
	ImGui::SameLine();
	if (g_sac_rebuilt ? ui::primary_button("Open Smart App Control settings") : ImGui::Button("Open Smart App Control settings")) settings();
	ImGui::SameLine(); if (ImGui::Button("Close")) { g_sac_open = g_sac_rebuilt = false; ImGui::CloseCurrentPopup(); }
	ImGui::EndPopup();
}

/* ---- setup: welcome, firmware, games, ready (first run, or whenever the firmware is missing) */
static bool g_setup_open;
static int g_setup_step;
static void stepper(int step) {
	static const char *const names[] = {"Welcome", "Firmware", "Your games", "Online", "Ready"};
	ImDrawList *dl = ImGui::GetWindowDrawList();
	float h = ImGui::GetTextLineHeight(), r = h * 0.5f;
	for (int i = 0; i < 5; i++) {
		if (i) ImGui::SameLine(0, h * 0.9f);
		ImVec2 p = ImGui::GetCursorScreenPos();
		bool done = i == 1 ? g_firmware : i < step, cur = i == step;   /* (firmware: done only when installed, skipped or not) */
		ImVec4 c = cur ? ui::accent : done ? ui::ready : ui::border;
		dl->AddCircleFilled(ImVec2(p.x + r, p.y + r), r, ui::u32(c));
		char n[4]; snprintf(n, sizeof n, "%d", i + 1);
		ImVec2 ts = ImGui::CalcTextSize(n);
		dl->AddText(ImVec2(p.x + r - ts.x * 0.5f, p.y + r - ts.y * 0.5f), ui::u32(cur || done ? ui::accent_ink : ui::muted), n);
		ImGui::Dummy(ImVec2(r * 2, h)); ImGui::SameLine(0, h * 0.35f);
		ImGui::PushStyleColor(ImGuiCol_Text, cur ? ui::text : ui::muted); ImGui::TextUnformatted(names[i]); ImGui::PopStyleColor();
	}
}
static void setup_modal(SDL_Window *win) {
	const ImGuiViewport *vp = ImGui::GetMainViewport();
	const float em = ImGui::GetFontSize();
	float w = vp->WorkSize.x < 44 * em ? vp->WorkSize.x * 0.94f : 40 * em;
	ImGui::SetNextWindowSize(ImVec2(w, 0), ImGuiCond_Always);
	ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	if (g_setup_open && !ImGui::IsPopupOpen("Setup")) ImGui::OpenPopup("Setup");
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(1.8f * em, 1.5f * em)); ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
	if (!ImGui::BeginPopupModal("Setup", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove)) { ImGui::PopStyleVar(2); return; }
	ImGui::PopStyleVar(2);
	stepper(g_setup_step);
	ImGui::Dummy(ImVec2(0, em * 0.6f));
	ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX());
	auto finish = [&]() { prefs_set("setup_done", "1"); g_setup_open = false; ImGui::CloseCurrentPopup(); };
	switch (g_setup_step) {
	case 0:
		{	/* the logo: the Vita, its screen dark, the name beneath (assets/brand/logo.svg) */
			ImDrawList *dl = ImGui::GetWindowDrawList(); const float avail = ImGui::GetContentRegionAvail().x, lw = std::min(em * 12, avail * 0.55f);
			ImVec2 p = ImGui::GetCursorScreenPos(); p.x += (avail - lw) * 0.5f;
			ui::vita(dl, p, lw, ui::u32(ui::text), ui::u32(ImVec4(0.051f, 0.059f, 0.075f, 1)));
			ui::wordmark(dl, ImVec2(p.x + lw * 0.25f, p.y + lw * 310 / 600), lw / 6);
			ImGui::Dummy(ImVec2(lw, lw * 420 / 600 + em * 0.4f));
		}
		ui::heading("Welcome to Project Nativ", em * 1.55f);
		ImGui::TextWrapped("Nativ rebuilds the PS Vita games you own as native programs for this computer — sharper, wider and faster than the console could run them.");
		ImGui::Dummy(ImVec2(0, em * 0.3f));
		ImGui::TextWrapped("It ships no Sony or game code. You bring two things, and they stay on this computer:");
		ImGui::Bullet(); ImGui::TextWrapped("The PS Vita system software (firmware) update file — free, from Sony's own website.");
		ImGui::Bullet(); ImGui::TextWrapped("Your own copies of your games and their updates: game folders copied from your Vita, or PKGs, or a whole folder to search.");
		ImGui::Bullet(); ImGui::TextWrapped("Their licences: a game folder from your Vita carries its own. For PKGs, Nativ fetches yours from your Vita over Wi-Fi, once.");
		ui::muted_wrapped("Each game plays as one exact release: the US version, at the versions its card lists (Killzone: 1.00 or its 1.12 update). Other regions and versions are recognised and named, but not supported yet.");
		ImGui::Dummy(ImVec2(0, em * 0.6f));
		if (ui::primary_button("Get started")) g_setup_step = 1;
		ImGui::SameLine(); if (ImGui::Button("Set up later")) finish();
		break;
	case 1:
		ui::heading("Firmware", em * 1.55f);
		if (g_firmware) {
			ui::dot(ui::ready); ImGui::TextWrapped("Installed (%s). Every game builds against it — nothing more to do here.", g_firmware_version.c_str());
		} else {
			ImGui::TextWrapped("Download the PS Vita system software update file from Sony's official page, then choose it below. Its system libraries are decrypted once and every game builds against them.");
			ImGui::Dummy(ImVec2(0, em * 0.2f));
			if (ImGui::Button("Open Sony's download page")) SDL_OpenURL(SONY_FIRMWARE_PAGE);
			ImGui::SameLine(); ImGui::TextLinkOpenURL("playstation.com/.../psvita/system-software", SONY_FIRMWARE_PAGE);
			ui::muted_wrapped("On that page, download the update file (PSVUPDAT.PUP or PSP2UPDAT.PUP, version 3.74). Any recent version works.");
			ImGui::Dummy(ImVec2(0, em * 0.2f));
			if (path_row("##pup", g_fw_path, "Choose the update file (.PUP)")) { static const SDL_DialogFileFilter pup[] = {{"PS Vita update file", "PUP;pup"}}; pick_file(win, &g_fw_path, pup, 1); }
			ImGui::BeginDisabled(g_fw_path.empty() || g_task.running);
			if (ui::primary_button("Install firmware")) task_start("Installing the firmware", "firmware", "", {step("Decrypting the system update", vitarecomp({"firmware", g_fw_path})), step("Installed")});
			ImGui::EndDisabled();
		}
		ImGui::Dummy(ImVec2(0, em * 0.2f)); task_line();
		ImGui::Dummy(ImVec2(0, em * 0.5f));
		if (g_task.running) ui::muted_wrapped("It carries on in the background: continue whenever you like.");
		if (g_firmware || g_task.running) { if (ui::primary_button("Continue")) g_setup_step = 2; }
		else if (ImGui::Button("Skip for now")) g_setup_step = 2;
		ImGui::SameLine(); if (ImGui::Button("Back")) g_setup_step = 0;
		break;
	case 2: {
		ui::heading("Your games", em * 1.55f);
		ImGui::TextWrapped("Add the games you own, with their updates. Each one is decrypted and checked against its game pack, which says which versions it plays. Your licences are kept once: every later PKG, update or add-on of those games just adds.");
		ImGui::Dummy(ImVec2(0, em * 0.2f));
		add_form(win);
		ImGui::Dummy(ImVec2(0, em * 0.2f)); task_line();
		ImGui::Dummy(ImVec2(0, em * 0.2f)); licences_row(win);
		int have = 0; for (const Game &g : g_games) have += g.imported;
		if (have) {
			ImGui::Dummy(ImVec2(0, em * 0.3f));
			ImGui::TextDisabled("In your library:");
			for (const Game &g : g_games) if (g.imported) { ImGui::Bullet(); ImGui::TextUnformatted(g.name.c_str()); }
		}
		ImGui::Dummy(ImVec2(0, em * 0.5f));
		if (g_task.running) ui::muted_wrapped("Adding and building carry on in the background: continue whenever you like.");
		if (have || g_task.running ? ui::primary_button("Continue") : ImGui::Button("Skip for now")) g_setup_step = 3;   /* (never waits on a running task: the bar at the bottom shows it) */
		ImGui::SameLine(); if (ImGui::Button("Back")) g_setup_step = 1;
		break;
	}
	case 3: {   /* the name other players see (the same field as Settings' Online name) */
		static std::string name; static bool loaded;
		if (!loaded) { g_shared = ini_load(shared_path()); name = online_name(g_shared); loaded = true; }
		ui::heading("Your online name", em * 1.55f);
		ImGui::TextWrapped("The name other players see when you play a game online. It shows as ntv_ and your name, so they know you're playing on PC.");
		ImGui::Dummy(ImVec2(0, em * 0.2f));
		ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("ntv_"); ImGui::SameLine(0, 0);
		input_text("##setup_oid", name, em * 12, "your name");
		const bool ok = online_name_ok(name);
		if (!ok) ui::muted_wrapped("Up to 10 letters, digits, '-' or '_'.");
		else if (name.empty()) ui::muted_wrapped(("Left empty, you play as ntv_" + online_tag(g_shared) + ", a tag made for this computer.").c_str());
		ui::muted_wrapped("You can change it any time in Settings.");
		ImGui::Dummy(ImVec2(0, em * 0.5f));
		{	static bool online = true; static bool online_loaded;
			if (!online_loaded) { const std::string *v = ini_find(g_shared, "online"); online = !v || atoi(v->c_str()) != 0; online_loaded = true; }
			ui::toggle("setup_online", &online); ImGui::SameLine(); ImGui::TextUnformatted("Play online");
			ui::muted_wrapped(ONLINE_EXPL);
			g_setup_online = online;
		}
		ImGui::Dummy(ImVec2(0, em * 0.5f));
		ImGui::BeginDisabled(!ok);
		if (ui::primary_button("Continue")) {
			if (g_settings_writable) { SetTarget st{&g_shared, shared_path(), SHARED_HEADER}; target_write(st, "online_id", name); target_write(st, "online", g_setup_online ? "1" : "0"); }
			g_setup_step = 4;
		}
		ImGui::EndDisabled();
		ImGui::SameLine(); if (ImGui::Button("Back")) g_setup_step = 2;
		break;
	}
	default:
		ui::heading("You’re set", em * 1.55f);
		ImGui::TextWrapped("Each game is built into a native program the first time — a few minutes per game — and starts straight away after that.");
		ImGui::Bullet(); ImGui::TextWrapped("In a game, F10 (or the PS button) opens the menu: display, controls, frame rate, field of view.");
		ImGui::Bullet(); ImGui::TextWrapped("Settings, at the top right, holds the display, controls and sound for every game; a card's … menu has that game's own options.");
		ImGui::Bullet(); ImGui::TextWrapped("Setup is at the top right whenever you need it again.");
		if (g_task.running) { ImGui::Dummy(ImVec2(0, em * 0.3f)); ui::muted_wrapped("Still working on your games: the bar at the bottom of the library shows how far it's got."); }
		ImGui::Dummy(ImVec2(0, em * 0.6f));
		if (ui::primary_button("Go to my library")) finish();
		ImGui::SameLine(); if (ImGui::Button("Back")) g_setup_step = 3;
		break;
	}
	ImGui::PopTextWrapPos();
	ImGui::EndPopup();
}

static bool g_add_open;   /* the Add a game dialog */
/* ---- a game's card: its cover (pic0), icon, name, state, one primary action */
static void game_card(const Game &g, float w, float name_h) {
	const float em = ImGui::GetFontSize(), pad = em * 0.9f;
	const float img_h = w * 544.0f / 960.0f;
	const bool busy = g_task.running, mine = busy && std::find(g_task.games.begin(), g_task.games.end(), g.id) != g_task.games.end();
	ImGui::PushID(g.id.c_str());
	ImGui::PushStyleColor(ImGuiCol_ChildBg, ui::card); ImGui::PushStyleColor(ImGuiCol_Border, ui::border);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
	ImGui::BeginChild("card", ImVec2(w, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY, ImGuiWindowFlags_NoScrollbar);
	ImDrawList *dl = ImGui::GetWindowDrawList();
	ImVec2 p = ImGui::GetCursorScreenPos();
	float rounding = ImGui::GetStyle().ChildRounding;
	Tex cover = g.cover.empty() ? Tex() : texture(g.cover), icon = g.icon.empty() ? Tex() : texture(g.icon);
	if (cover.t) {   /* filled, not stretched: a LiveArea background is 840x500, the card 960x544 */
		ImVec2 uv0(0, 0), uv1(1, 1); float box = w / img_h, ia = (float)cover.w / (float)cover.h;
		if (ia > box) { uv0.x = (1 - box / ia) * 0.5f; uv1.x = 1 - uv0.x; } else { uv0.y = (1 - ia / box) * 0.5f; uv1.y = 1 - uv0.y; }
		dl->AddImageRounded(tex_ref(cover), p, ImVec2(p.x + w, p.y + img_h), uv0, uv1, IM_COL32_WHITE, rounding, ImDrawFlags_RoundCornersTop);
	}
	else {   /* no art in this copy: a quiet panel with the title set large */
		dl->AddRectFilledMultiColor(p, ImVec2(p.x + w, p.y + img_h), ui::u32(ui::card_hi), ui::u32(ui::card_hi), ui::u32(ui::surface), ui::u32(ui::surface));
		ImGui::PushFont(nullptr, em * 1.9f);
		float wrap = w - em * 3;
		ImVec2 ts = ImGui::CalcTextSize(g.name.c_str(), nullptr, false, wrap);
		dl->AddText(nullptr, 0, ImVec2(p.x + (w - ts.x) * 0.5f, p.y + (img_h - ts.y) * 0.5f), ui::u32(ui::muted, 0.75f), g.name.c_str(), nullptr, wrap);
		ImGui::PopFont();
	}
	if (icon.t) {   /* the icon over the cover's bottom-left corner */
		float s = em * 3.0f; ImVec2 a(p.x + pad, p.y + img_h - s * 0.6f);
		dl->AddRectFilled(ImVec2(a.x - 2, a.y - 2), ImVec2(a.x + s + 2, a.y + s + 2), ui::u32(ui::card), em * 0.5f);
		dl->AddImageRounded(tex_ref(icon), a, ImVec2(a.x + s, a.y + s), ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, em * 0.45f);
	}
	ImGui::Dummy(ImVec2(w, img_h + em * 2.0f));   /* (room for the icon on every card, so they line up) */
	/* the text block, one height on every card: name (two lines at most), state, title ID */
	float y0 = ImGui::GetCursorPosY();
	ImGui::SetCursorPosX(pad);
	ImGui::PushTextWrapPos(w - pad);
	ImGui::PushFont(nullptr, em * 1.15f); ImGui::TextWrapped("%s", g.name.c_str()); ImGui::PopFont();
	ImGui::SetCursorPosY(y0 + name_h + ImGui::GetStyle().ItemSpacing.y);
	ImGui::SetCursorPosX(pad);
	if (mine) { ui::spinner(ui::accent); ImGui::TextDisabled("%s", task_status().c_str()); }
	else if (g.built) { ui::dot(ui::ready); ImGui::TextUnformatted("Ready to play"); }
	else if (g.imported) { ui::dot(ui::warn); ImGui::TextUnformatted("Needs a one-time build"); }
	else { ui::dot(ui::border); ImGui::TextDisabled("Not added yet"); }
	ImGui::SetCursorPosX(pad);
	{	/* the title id and the version: this copy's, else what the pack plays; a chip when the pack recommends another */
		std::string line = g.id, rec = recommended_version(g), vs = played_versions(g);
		if (!g.pack) line += "  -  experimental (no game pack)";
		else if (g.imported && !g.version.empty() && g.version != "-") line += "  ·  " + (g.region.empty() ? "" : g.region + " ") + g.version;
		else if (!g.imported && !vs.empty()) line += "  ·  plays " + (g.region.empty() ? "" : g.region + " ") + vs;
		ImGui::TextDisabled("%s", line.c_str());
		if (g.imported && !rec.empty() && g.version != rec) {
			ImGui::SameLine();   /* (one unwrapped line of text, not a chip: every card's lines keep one height) */
			ImGui::PushTextWrapPos(-1.0f); ImGui::TextColored(ui::warn, "Update %s", rec.c_str()); ImGui::PopTextWrapPos();
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("%s (this copy is %s).\nAdd it with + Add game: its update folder (ux0:patch/%s) or PKG,\nor search the folder you copied from your Vita.",
				                  (g.update_note.empty() ? "Version " + rec + " is recommended" : g.update_note).c_str(), g.version.c_str(), g.id.c_str());
		}
	}
	ImGui::PopTextWrapPos();
	ImGui::Dummy(ImVec2(0, em * 0.2f));
	ImGui::SetCursorPosX(pad);
	float bw = w - pad * 2, more = ImGui::GetFrameHeight();
	ImGui::BeginDisabled(busy);
	if (g.built) { if (ui::primary_button("Play", ImVec2(bw - more - ImGui::GetStyle().ItemSpacing.x, 0))) start_play(g); ImGui::SameLine(); }
	else if (g.imported) { if (ui::primary_button("Build", ImVec2(bw - more - ImGui::GetStyle().ItemSpacing.x, 0))) start_build(g); ImGui::SameLine(); }
	else { if (ImGui::Button("Add your copy…", ImVec2(bw - more - ImGui::GetStyle().ItemSpacing.x, 0))) g_add_open = true; ImGui::SameLine(); }
	ImGui::EndDisabled();
	{	/* the "more" button: three dots, drawn (the font may have no ellipsis glyph) */
		if (ImGui::Button("##more", ImVec2(more, 0))) ImGui::OpenPopup("more");
		ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax(), m((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
		for (int k = -1; k <= 1; k++) dl->AddCircleFilled(ImVec2(m.x + k * em * 0.32f, m.y), em * 0.09f, ui::u32(ui::text, 0.8f));
	}
	if (ImGui::BeginPopup("more")) {
		ImGui::BeginDisabled(busy || !g.imported);
		if (ImGui::MenuItem(g.built ? "Rebuild" : "Build")) start_build(g);
		ImGui::EndDisabled();
		ImGui::BeginDisabled(!g.imported);
		if (ImGui::MenuItem("Game settings…")) { make_dir(g_dump + "/decrypted/" + g.id + "/recomp"); g_game_settings = g.id; }
		ImGui::EndDisabled();
		if (ImGui::MenuItem("Show its folder")) open_folder(g_dump + "/decrypted/" + g.id);
		ImGui::BeginDisabled(busy);
		if (ImGui::MenuItem("Report a problem…"))   /* (the logs, settings and F5 pictures in one zip, shown in its folder) */
			task_start("Making a bug report for " + g.name, "report", g.id, {step("Collecting the logs and settings", vitarecomp({"report", g.id}))});
		ImGui::EndDisabled();
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("One zip of this game's logs, your settings and the pictures from your last F5,\nto send with a description of what happened. No game files, saves or keys.");
		ImGui::EndPopup();
	}
	ImGui::Dummy(ImVec2(0, pad * 0.4f));
	ImGui::EndChild();
	ImGui::PopStyleVar(); ImGui::PopStyleColor(2);
	ImGui::PopID();
}

/* ---- the window */
static void hub_ui(SDL_Window *win) {
	const ImGuiViewport *vp = ImGui::GetMainViewport();
	const float em = ImGui::GetFontSize();
	ImGui::SetNextWindowPos(vp->WorkPos); ImGui::SetNextWindowSize(vp->WorkSize);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f); ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
	ImGui::Begin("##hub", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
	ImGui::PopStyleVar(2);
	const bool busy = g_task.running, narrow = vp->WorkSize.x < 44 * em;

	/* header: the logo (the Vita, its screen lit as the app icon's, and the wordmark), the setup state, Add game, the setup gear */
	{	ImDrawList *dl = ImGui::GetWindowDrawList(); const ImVec2 p = ImGui::GetCursorScreenPos(); const float h = em * 1.6f, vh = h * 0.9f, vw = vh * 600 / 280;
		ui::vita(dl, ImVec2(p.x, p.y + (h - vh) * 0.5f), vw, ui::u32(ui::text), ui::u32(ui::accent), 30, 1);
		float ww = ui::wordmark(dl, ImVec2(p.x + vw + em * 0.6f, p.y), h);
		ImGui::Dummy(ImVec2(vw + em * 0.6f + ww, h));
	}
	if (!narrow) { ImGui::SameLine(); ImGui::SetCursorPosY(ImGui::GetCursorPosY() + em * 0.45f); ImGui::TextDisabled("your PS Vita games, rebuilt natively"); }
	float right = ImGui::GetWindowContentRegionMax().x;
	float bw = ImGui::CalcTextSize("+ Add game").x + ImGui::CalcTextSize("Settings").x + ImGui::CalcTextSize("Setup").x + ImGui::GetStyle().FramePadding.x * 6 + ImGui::GetStyle().ItemSpacing.x * 2;
	std::string fw = g_firmware ? "Firmware " + g_firmware_version : "Firmware missing";
	float pw = ImGui::CalcTextSize(fw.c_str()).x + em * 1.6f;
	if (!narrow) ImGui::SameLine(right - bw - pw - ImGui::GetStyle().ItemSpacing.x * 2); else ImGui::Dummy(ImVec2(0, 0));
	{	/* the firmware pill: green when installed, amber (and opens setup) when not */
		ImVec2 p = ImGui::GetCursorScreenPos(); float h = ImGui::GetFrameHeight();
		ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + pw, p.y + h), ui::u32(g_firmware ? ui::ready : ui::warn, 0.14f), h * 0.5f);
		ImDrawList *dl = ImGui::GetWindowDrawList(); float lh = ImGui::GetTextLineHeight();
		dl->AddCircleFilled(ImVec2(p.x + em * 0.6f + lh * 0.22f, p.y + h * 0.5f), lh * 0.22f, ui::u32(g_firmware ? ui::ready : ui::warn));
		dl->AddText(ImVec2(p.x + em * 0.6f + lh * 0.8f, p.y + (h - lh) * 0.5f), ui::u32(ui::text), fw.c_str());
		if (ImGui::InvisibleButton("fwpill", ImVec2(pw, h)) && !g_firmware) { g_setup_step = 1; g_setup_open = true; }
		if (ImGui::IsItemHovered() && !g_firmware) ImGui::SetTooltip("Install the firmware");
	}
	ImGui::SameLine();
	ImGui::BeginDisabled(busy);
	if (ui::primary_button("+ Add game")) g_add_open = true;
	ImGui::EndDisabled();
	ImGui::SameLine(); if (ImGui::Button("Settings")) g_settings_open = true;
	ImGui::SameLine(); if (ImGui::Button("Setup")) { g_setup_step = 0; g_setup_open = true; }
	ImGui::Dummy(ImVec2(0, em * 0.3f));

	/* the library: a grid of cards, sized to the window */
	const float bar_h = ImGui::GetFrameHeightWithSpacing() + em * 0.8f, drawer_h = g_drawer ? ImGui::GetContentRegionAvail().y * 0.4f : 0;
	ImGui::BeginChild("library", ImVec2(0, ImGui::GetContentRegionAvail().y - bar_h - drawer_h), 0);
	int nimp = 0; for (const Game &g : g_games) nimp += g.imported;
	ImGui::PushFont(nullptr, em * 1.15f); ImGui::TextUnformatted("Library"); ImGui::PopFont();
	ImGui::SameLine(); ImGui::TextDisabled("%d of %zu added", nimp, g_games.size());
	if (!g_firmware) {
		ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(ui::warn.x, ui::warn.y, ui::warn.z, 0.10f));
		ImGui::BeginChild("fwnote", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
		ui::dot(ui::warn); ImGui::TextWrapped("The firmware isn't installed yet. Games build against Sony's system software — it’s a free download from Sony.");
		if (ui::primary_button("Set up the firmware")) { g_setup_step = 1; g_setup_open = true; }
		ImGui::EndChild(); ImGui::PopStyleColor();
	}
	{	std::string tag, page; { std::lock_guard<std::mutex> l(g_update.m); if (!g_update.dismissed) { tag = g_update.tag; page = g_update.page; } }
		if (!tag.empty()) {   /* a newer release: its page, where the download is */
			ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(ui::accent.x, ui::accent.y, ui::accent.z, 0.10f));
			ImGui::BeginChild("updnote", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
#ifdef __APPLE__
			ui::dot(ui::accent); ImGui::TextWrapped("Project Nativ %s is out. Download it and drag it into Applications, replacing this one: your games, saves and settings stay as they are.", tag.c_str());
#else
			ui::dot(ui::accent); ImGui::TextWrapped("Project Nativ %s is out. Download it and replace this folder with it: your games, saves and settings stay as they are.", tag.c_str());
#endif
			if (ui::primary_button("Download")) SDL_OpenURL(page.c_str());
			ImGui::SameLine(); if (ImGui::Button("Not now")) { std::lock_guard<std::mutex> l(g_update.m); g_update.dismissed = true; }
			ImGui::EndChild(); ImGui::PopStyleColor();
		}
	}
	if (g_games.empty()) {
		ImGui::Dummy(ImVec2(0, em * 2));
		ImGui::TextDisabled("No games yet."); ImGui::TextWrapped("Add your own copy of a game — a PKG with its licence file, or a game folder — with + Add game.");
	}
	const float gap = em * 1.0f, min_w = em * 15.5f, avail = ImGui::GetContentRegionAvail().x;
	int cols = (int)((avail + gap) / (min_w + gap)); if (cols < 1) cols = 1;
	float cw = (avail - gap * (cols - 1)) / cols; if (cw > em * 24) cw = em * 24;   /* (a wide window gets more columns, not giant cards) */
	float name_h = 0;   /* the longest name's height at this width: every card's text block lines up */
	ImGui::PushFont(nullptr, em * 1.15f);
	for (const Game &g : g_games) { float hh = ImGui::CalcTextSize(g.name.c_str(), nullptr, false, cw - em * 1.8f).y; if (hh > name_h) name_h = hh; }
	ImGui::PopFont();
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, gap));
	for (size_t i = 0; i < g_games.size(); i++) {
		if (i % cols) ImGui::SameLine();
		ImGui::BeginGroup(); game_card(g_games[i], cw, name_h); ImGui::EndGroup();
	}
	ImGui::PopStyleVar();
	ImGui::EndChild();

	/* the activity bar: what's running (its step), the last failure, the log on demand */
	ImGui::Separator();
	ImGui::Dummy(ImVec2(0, em * 0.1f));
	if (busy || !g_failed.empty()) task_line(); else { ui::dot(ui::border); ImGui::TextDisabled("Nothing running"); }
	{
		const ImGuiStyle &sty = ImGui::GetStyle();
		float lw = ImGui::CalcTextSize(g_drawer ? "Hide log" : "Show log").x + sty.FramePadding.x * 2, cw2 = busy ? ImGui::CalcTextSize("Cancel").x + sty.FramePadding.x * 2 + sty.ItemSpacing.x : 0;
		const bool tip = !busy && !narrow; const float sw = tip ? ImGui::CalcTextSize("Support Nativ").x + sty.ItemSpacing.x * 3 : 0;
		ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - lw - cw2 - sw);
		if (tip) {   /* a quiet link to the developer's tip jar: never a popup, never in a game */
			ImGui::AlignTextToFramePadding(); ImGui::PushStyleColor(ImGuiCol_TextLink, ui::muted);
			if (ImGui::TextLink("Support Nativ")) SDL_OpenURL(SUPPORT_PAGE);
			ImGui::PopStyleColor();
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Nativ is free. If you'd like to support its development, you can leave a tip on Ko-fi.");
			ImGui::SameLine(0, sty.ItemSpacing.x * 2);
		}
	}
	if (busy) { if (ImGui::Button("Cancel")) task_cancel(); ImGui::SameLine(); }
	if (ImGui::Button(g_drawer ? "Hide log" : "Show log")) g_drawer = !g_drawer;
	if (g_drawer) {
		ImGui::PushStyleColor(ImGuiCol_ChildBg, ui::surface);
		ImGui::BeginChild("log", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
		{ std::lock_guard<std::mutex> l(g_log_lock); ImGui::PushStyleColor(ImGuiCol_Text, ui::muted); ImGui::TextUnformatted(g_log.c_str(), g_log.c_str() + g_log.size()); ImGui::PopStyleColor(); }
		if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.0f);   /* follow the end unless scrolled up */
		ImGui::EndChild();
		ImGui::PopStyleColor();
	}

	/* Add a game, as its own dialog */
	if (g_add_open && !ImGui::IsPopupOpen("Add a game")) ImGui::OpenPopup("Add a game");
	ImGui::SetNextWindowSize(ImVec2(narrow ? vp->WorkSize.x * 0.94f : 38 * em, 0), ImGuiCond_Always);
	ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(1.6f * em, 1.3f * em)); ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
	if (ImGui::BeginPopupModal("Add a game", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove)) {
		ImGui::PopStyleVar(2);
		ui::heading("Add a game", em * 1.4f);
		ImGui::PushTextWrapPos(0);
		if (add_form(win)) { g_add_open = false; ImGui::CloseCurrentPopup(); }
		ImGui::SameLine(); if (ImGui::Button("Cancel")) { g_add_open = false; ImGui::CloseCurrentPopup(); }
		ImGui::Dummy(ImVec2(0, em * 0.3f)); ImGui::Separator(); ImGui::Dummy(ImVec2(0, em * 0.1f)); licences_row(win);
		ImGui::PopTextWrapPos();
		ImGui::EndPopup();
	} else ImGui::PopStyleVar(2);
	sac_modal();
	online_name_modal();
	setup_modal(win);
	settings_modal();
	game_settings_modal();
	ImGui::End();
}

/* ---- ATIV_SHOT: one frame into a file, no visible window */
static int shot(SDL_Window *win, const char *path, int w, int h) {
	SDL_GPUTextureCreateInfo ci = {};
	ci.type = SDL_GPU_TEXTURETYPE_2D; ci.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM; ci.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
	ci.width = (Uint32)w; ci.height = (Uint32)h; ci.layer_count_or_depth = 1; ci.num_levels = 1;
	SDL_GPUTexture *t = SDL_CreateGPUTexture(g_gpu, &ci);
	SDL_GPUTransferBufferCreateInfo dc = {}; dc.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD; dc.size = (Uint32)(w * h * 4);
	SDL_GPUTransferBuffer *down = SDL_CreateGPUTransferBuffer(g_gpu, &dc);
	if (!t || !down) { fprintf(stderr, "ATIV_SHOT: out of GPU memory\n"); return 1; }
	const int frames = 40;
	for (int f = 0; f < frames; f++) {   /* ImGui sizes auto-sized windows on the first frames; a modal's dim fades in */
		ImGui_ImplSDLGPU3_NewFrame(); ImGui_ImplSDL3_NewFrame();
		ImGui::GetIO().DisplaySize = ImVec2((float)w, (float)h); ImGui::GetIO().DisplayFramebufferScale = ImVec2(1, 1); ImGui::GetIO().DeltaTime = 1.0f / 60;
		ImGui::NewFrame(); hub_ui(win); ImGui::Render();
		ImDrawData *dd = ImGui::GetDrawData();
		SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(g_gpu);
		ImGui_ImplSDLGPU3_PrepareDrawData(dd, cmd);
		SDL_GPUColorTargetInfo ti = {}; ti.texture = t; ti.clear_color = SDL_FColor{ui::bg.x, ui::bg.y, ui::bg.z, 1}; ti.load_op = SDL_GPU_LOADOP_CLEAR; ti.store_op = SDL_GPU_STOREOP_STORE;
		SDL_GPURenderPass *rp = SDL_BeginGPURenderPass(cmd, &ti, 1, nullptr);
		ImGui_ImplSDLGPU3_RenderDrawData(dd, cmd, rp);
		SDL_EndGPURenderPass(rp);
		if (f == frames - 1) {
			SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
			SDL_GPUTextureRegion src = {}; src.texture = t; src.w = (Uint32)w; src.h = (Uint32)h; src.d = 1;
			SDL_GPUTextureTransferInfo dst = {}; dst.transfer_buffer = down; dst.pixels_per_row = (Uint32)w; dst.rows_per_layer = (Uint32)h;
			SDL_DownloadFromGPUTexture(cp, &src, &dst);
			SDL_EndGPUCopyPass(cp);
		}
		SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
		SDL_WaitForGPUFences(g_gpu, true, &fence, 1); SDL_ReleaseGPUFence(g_gpu, fence);
	}
	const uint8_t *px = (const uint8_t *)SDL_MapGPUTransferBuffer(g_gpu, down, false);
	FILE *f = fopen(path, "wb");
	if (!f || !px) { fprintf(stderr, "ATIV_SHOT: can't write %s\n", path); return 1; }
	fprintf(f, "P6 %d %d 255\n", w, h);
	for (int i = 0; i < w * h; i++) fwrite(px + 4 * i, 1, 3, f);
	fclose(f); SDL_UnmapGPUTransferBuffer(g_gpu, down);
	fprintf(stderr, "ATIV_SHOT: %s\n", path);
	return 0;
}

/* the repository (or an install of it): ATIV_ROOT, else the one the launcher sits in -- launcher/nativ, or beside it
 * at the top -- wherever it was moved, else where it was built */
static std::string find_root(void) {
	if (const char *r = getenv("ATIV_ROOT")) if (*r) return r;
	if (const char *b = SDL_GetBasePath())
		for (const char *up : {"..", "."}) {
			std::string r = std::string(b) + up;
#ifdef _WIN32
			char full[MAX_PATH * 4]; if (_fullpath(full, r.c_str(), sizeof full)) r = full;
#else
			if (char *full = realpath(r.c_str(), nullptr)) { r = full; free(full); }
#endif
			while (r.size() > 1 && (r.back() == '/' || r.back() == '\\')) r.pop_back();
			if (exists(r + "/pipeline/vitarecomp.py")) return r;
		}
	return REPO_ROOT;
}

/* the player's library (pipeline/datadir.py makes the same choice; the stages get this one as ATIV_DUMP): ATIV_DUMP, else
 * <root>/dump when it exists (a checkout, an install from before), else the platform's per-user data folder -- local, not
 * roaming, on Windows: a game is gigabytes -- so an update replaces the install without touching what the player made */
static std::string library_dir(void) {
	if (const char *d = getenv("ATIV_DUMP")) if (*d) return d;
	if (exists(g_root + "/dump")) return g_root + "/dump";
	const char *home = getenv("HOME"); std::string base;
#ifdef _WIN32
	const char *la = getenv("LOCALAPPDATA"), *up = getenv("USERPROFILE");
	base = la && *la ? la : std::string(up ? up : ".") + "/AppData/Local";
#elif defined(__APPLE__)
	base = std::string(home ? home : ".") + "/Library/Application Support";
#else
	const char *xdg = getenv("XDG_DATA_HOME");
	base = xdg && *xdg ? xdg : std::string(home ? home : ".") + "/.local/share";
#endif
	(void)home;
	return base + "/ProjectNativ/Nativ/library";
}

static int task_cli(const std::string &t) {   /* ATIV_TASK */
	size_t c = t.find(':'); std::string kind = t.substr(0, c), arg = c == std::string::npos ? "" : t.substr(c + 1);
	library_scan();
	const Game *g = nullptr; for (const Game &x : g_games) if (x.id == arg) g = &x;
	if ((kind == "play" || kind == "build") && !g) { fprintf(stderr, "ATIV_TASK: no game %s in the library\n", arg.c_str()); return 2; }
	if (kind == "play") start_play(*g);
	else if (kind == "build") start_build(*g);
	else if (kind == "scan") task_start("Searching a folder", "scan", "", {step("Searching", vitarecomp({"scan", arg, "--out", scan_file()}))});
	else if (kind == "licences") task_start("Importing licences", "licences", "", {step("", vitarecomp({"licenses", "import", arg}))});
	else if (kind == "firmware") task_start("Installing the firmware", "firmware", "", {step("Decrypting the system update", vitarecomp({"firmware", arg})), step("Installed")});
	else { fprintf(stderr, "ATIV_TASK: play|build|scan|licences|firmware\n"); return 2; }
	const char *cancel = getenv("ATIV_TASK_CANCEL");
	Uint64 t0 = SDL_GetTicks();
	while (g_task.running) {
		SDL_Delay(50);
		if (cancel && SDL_GetTicks() - t0 > (Uint64)(atof(cancel) * 1000)) { task_cancel(); cancel = nullptr; }
	}
	g_task.reader.join();
#ifdef _WIN32
	_setmode(_fileno(stdout), _O_BINARY);   /* (the log as the steps wrote it: no second \r before each \n) */
#endif
	fwrite(g_log.data(), 1, g_log.size(), stdout);
	if (kind == "scan" && g_task.ok) { scan_load(scan_file()); printf("(%zu found)\n", g_found.size()); }
	return g_task.ok ? 0 : 1;
}

#ifdef __APPLE__
/* the Mac app's own checks, before its window: where it runs from (a temporary copy macOS makes of a downloaded app
 * opened where it landed, or a read-only disk image: it belongs in Applications), the download's quarantine on its own
 * files (cleared once the player has opened it), and Apple's Command Line Tools (every game build needs their compiler
 * and SDK: Apple's to install, not ours to ship). False: it said why and the launcher stops. */
static bool mac_preflight(void) {
	struct statfs fs;
	const bool in_place = g_root.find("/AppTranslocation/") == std::string::npos && !(statfs(g_root.c_str(), &fs) == 0 && (fs.f_flags & MNT_RDONLY));
	if (!in_place) {
		const SDL_MessageBoxButtonData b[2] = {{SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Open Applications"}, {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Quit"}};
		const SDL_MessageBoxData m = {SDL_MESSAGEBOX_INFORMATION, nullptr, "Project Nativ", "Drag Project Nativ into your Applications folder first, then open it from there.", 2, b, nullptr};
		int hit = 0; SDL_ShowMessageBox(&m, &hit);
		if (hit == 1) SDL_OpenURL("file:///Applications");
		return false;
	}
	const size_t c = g_root.rfind("/Contents/Resources");   /* (inside the app: its bundle) */
	const std::string app = c != std::string::npos ? g_root.substr(0, c) : g_root;
	if (getxattr(app.c_str(), "com.apple.quarantine", nullptr, 0, 0, 0) >= 0) (void)!system(("xattr -dr com.apple.quarantine '" + app + "' 2>/dev/null").c_str());
	if (system("xcode-select -p >/dev/null 2>&1") != 0) {
		(void)!system("xcode-select --install >/dev/null 2>&1");
		SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "Project Nativ", "Project Nativ builds each game on your Mac with Apple's free Command Line Tools.\n\nInstall them from the window that just opened, then open Project Nativ again.", nullptr);
		return false;
	}
	return true;
}
#endif

int main(int, char **) {
	g_root = find_root();
	if (!exists(g_root + "/pipeline/vitarecomp.py")) {   /* (the launcher apart from its folder: run from inside the zip, or copied out alone) */
		SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Project Nativ", "Project Nativ can't find the rest of its files.\n\nIf you opened it from inside the zip, extract the whole zip first (on Windows: right-click it, Extract All), then open Project Nativ from the extracted folder.", nullptr);
		return 1;
	}
	g_dump = library_dir();
	set_env("ATIV_DUMP", g_dump);   /* (the stages it runs: the same library whatever their own rule finds) */
	const char *shot_path = getenv("ATIV_SHOT");
#ifdef SIGPIPE
	signal(SIGPIPE, SIG_IGN);
#endif
#ifdef __APPLE__
	if (!shot_path && !getenv("ATIV_TASK") && !mac_preflight()) return 0;
#endif
	if (!getenv("PYTHONPYCACHEPREFIX")) set_env("PYTHONPYCACHEPREFIX", pref_dir() + "pycache");   /* (the stages' compiled Python beside the settings: an install is never written) */
#if !defined(_WIN32) && !defined(__APPLE__)
	if (!getenv("ZIG_GLOBAL_CACHE_DIR")) {   /* (Linux's compiler's own cache, kept per user: an AppImage's install can't be written, and it outlives an update) */
		const char *x = getenv("XDG_CACHE_HOME"), *h = getenv("HOME");
		set_env("ZIG_GLOBAL_CACHE_DIR", (x && *x ? std::string(x) : std::string(h ? h : "/tmp") + "/.cache") + "/ProjectNativ/zig");
	}
#endif
	if (const char *t = getenv("ATIV_TASK")) return task_cli(t);   /* (after the caches above: a task's steps run as the window's do) */
	if (!shot_path) {   /* (the last launcher's log kept beside this one's) */
		const std::string d = pref_dir();
		if (!d.empty()) { g_log_file = d + "launcher.log"; remove((d + "launcher.prev.log").c_str()); rename(g_log_file.c_str(), (d + "launcher.prev.log").c_str()); }
	}
	if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1; }
	int sw = 1280, sh = 800;
	if (const char *s = getenv("ATIV_SHOT_SIZE")) sscanf(s, "%dx%d", &sw, &sh);
	const float scale = shot_path ? 1.0f : SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
	SDL_Window *win = shot_path ? SDL_CreateWindow("Project Nativ", sw, sh, SDL_WINDOW_HIDDEN)
	                            : SDL_CreateWindow("Project Nativ", (int)(1180 * scale), (int)(760 * scale), SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
	g_gpu = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_METALLIB, false, nullptr);
	if (!win || !g_gpu || (!shot_path && !SDL_ClaimWindowForGPUDevice(g_gpu, win))) { fprintf(stderr, "window/GPU: %s\n", SDL_GetError()); return 1; }
	if (!shot_path) SDL_SetGPUSwapchainParameters(g_gpu, win, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, SDL_GPU_PRESENTMODE_VSYNC);
	SDL_SetWindowMinimumSize(win, 420, 480);
	window_icon(win);

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO &io = ImGui::GetIO();
	io.IniFilename = nullptr;
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;   /* a controller (or the arrows) moves through everything; L1/R1 the tabs */
	ui::load_system_font(io);
	ui::apply(16.0f, scale);
	ImGui_ImplSDL3_InitForSDLGPU(win);
	ImGui_ImplSDLGPU3_InitInfo info = {};
	info.Device = g_gpu; info.ColorTargetFormat = shot_path ? SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM : SDL_GetGPUSwapchainTextureFormat(g_gpu, win); info.MSAASamples = SDL_GPU_SAMPLECOUNT_1;
	ImGui_ImplSDLGPU3_Init(&info);

	prefs_load();
	g_settings_writable = !shot_path || getenv("ATIV_PREFS");
	if (g_settings_writable) settings_migrate();   /* (a screenshot run touches no one's settings but a test folder's) */
	set_env("RECOMP_SETTINGS_SHARED", shared_path());   /* the games it starts read the same shared file (runtime/core/main.c) */
	library_scan();
	g_setup_open = g_prefs["setup_done"] != "1" || !g_firmware;   /* first run, or the firmware still missing */
	if (!g_firmware && g_prefs["setup_done"] == "1") g_setup_step = 1;
	if (shot_path) {
		std::string ui_sel = getenv("ATIV_SHOT_UI") ? getenv("ATIV_SHOT_UI") : "library";
		g_setup_open = ui_sel.rfind("setup", 0) == 0;
		if (g_setup_open) g_setup_step = ui_sel.size() > 6 ? atoi(ui_sel.c_str() + 6) : 0;
		g_add_open = ui_sel == "add" || ui_sel == "add:vita"; g_open_vita_popup = ui_sel == "add:vita";
		if (const char *f = getenv("ATIV_SHOT_SCAN")) { scan_load(f); g_add_kind = 0; g_scan_path = "/Volumes/Vita/ux0"; }
		g_drawer = ui_sel == "log";
		g_sac_open = ui_sel.rfind("sac", 0) == 0; g_sac_game = "PCSE00640"; g_sac_rebuilt = ui_sel == "sac:again";
		g_settings_open = ui_sel.rfind("settings", 0) == 0; if (g_settings_open && ui_sel.size() > 9) g_settings_tab = atoi(ui_sel.c_str() + 9);
		if (ui_sel.rfind("game:", 0) == 0) g_game_settings = ui_sel.substr(5);
		if (ui_sel.rfind("name:", 0) == 0) g_name_game = ui_sel.substr(5);
		if (getenv("ATIV_SHOT_NOFW") && *getenv("ATIV_SHOT_NOFW")) { g_firmware = false; g_firmware_version.clear(); }
		if (const char *u = getenv("ATIV_SHOT_UPDATE")) { g_update.tag = u; g_update.page = "https://example.invalid/"; }   /* (the notice, without asking anyone) */
		int r = shot(win, shot_path, sw, sh);
		SDL_WaitForGPUIdle(g_gpu);
		ImGui_ImplSDL3_Shutdown(); ImGui_ImplSDLGPU3_Shutdown(); ImGui::DestroyContext();
		SDL_DestroyGPUDevice(g_gpu); SDL_DestroyWindow(win); SDL_Quit();
		return r;
	}

	update_check_start();
	bool quit = false;
	while (!quit) {
		SDL_Event e;
		while (SDL_PollEvent(&e)) {
			ImGui_ImplSDL3_ProcessEvent(&e);
			if (e.type == SDL_EVENT_QUIT || (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && e.window.windowID == SDL_GetWindowID(win))) quit = true;
		}
		if (SDL_GetWindowFlags(win) & SDL_WINDOW_MINIMIZED) { SDL_Delay(50); continue; }
		pick_poll();
		{	/* a touch screen in use: bigger targets (finger-sized frame padding), until a mouse moves again */
			static bool touch; static ImVec2 pad = ImGui::GetStyle().FramePadding;
			bool t = io.MouseSource == ImGuiMouseSource_TouchScreen;   /* (the SDL backend tags touch-driven pointer events) */
			if (t != touch) { touch = t; ImGui::GetStyle().FramePadding = t ? ImVec2(pad.x * 1.4f, pad.y * 1.8f) : pad; }
		}
		static Uint64 last_scan;
		if (g_task.running && SDL_GetTicks() - last_scan > 1500) { library_scan(); last_scan = SDL_GetTicks(); }   /* (a long add: its card follows) */
		if (g_refresh.exchange(false)) {
			if (g_task.kind == "play") { SDL_RestoreWindow(win); SDL_RaiseWindow(win); }   /* (the game closed: back to the library) */
			library_scan();
			if (g_task.kind == "scan" && g_task.ok) scan_load(scan_file());
			if (g_task.kind == "report" && g_task.ok) open_folder(g_dump + "/reports");
			if (g_task.kind == "play") {   /* Windows refused the game (vitarecomp's SAC_BLOCKED line): the Smart App Control dialog */
				bool blocked;
				{ std::lock_guard<std::mutex> l(g_log_lock); blocked = !g_task.ok && g_log.find("Smart App Control is on", g_task.log_from) != std::string::npos; }
				if (blocked && g_sac_game != g_task.game) g_sac_rebuilt = false;   /* (another game: its first refusal) */
				if (blocked) { g_sac_game = g_task.game; g_sac_open = true; } else g_sac_rebuilt = false;
			}
		}
		ImGui_ImplSDLGPU3_NewFrame(); ImGui_ImplSDL3_NewFrame(); ImGui::NewFrame();
		hub_ui(win);
		ImGui::Render();
		ImDrawData *dd = ImGui::GetDrawData();
		SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(g_gpu);
		SDL_GPUTexture *swap = nullptr;
		if (cmd && SDL_WaitAndAcquireGPUSwapchainTexture(cmd, win, &swap, nullptr, nullptr) && swap && dd->DisplaySize.x > 0) {
			ImGui_ImplSDLGPU3_PrepareDrawData(dd, cmd);
			SDL_GPUColorTargetInfo ti = {};
			ti.texture = swap; ti.clear_color = SDL_FColor{ui::bg.x, ui::bg.y, ui::bg.z, 1}; ti.load_op = SDL_GPU_LOADOP_CLEAR; ti.store_op = SDL_GPU_STOREOP_STORE;
			SDL_GPURenderPass *rp = SDL_BeginGPURenderPass(cmd, &ti, 1, nullptr);
			ImGui_ImplSDLGPU3_RenderDrawData(dd, cmd, rp);
			SDL_EndGPURenderPass(rp);
		}
		if (cmd) SDL_SubmitGPUCommandBuffer(cmd);
	}
	task_cancel();
	if (g_task.reader.joinable()) g_task.reader.join();
	SDL_WaitForGPUIdle(g_gpu);
	for (auto &kv : g_tex) if (kv.second.t) SDL_ReleaseGPUTexture(g_gpu, kv.second.t);
	ImGui_ImplSDL3_Shutdown(); ImGui_ImplSDLGPU3_Shutdown(); ImGui::DestroyContext();
	SDL_ReleaseWindowFromGPUDevice(g_gpu, win); SDL_DestroyGPUDevice(g_gpu); SDL_DestroyWindow(win); SDL_Quit();
	return 0;
}
