/*
 * runtime/gfx/mtl_archive.m's sweep of Metal's scratch folders (macOS). Metal gives every binary archive a pipeline is
 * added to a folder, com.apple.gpuarchiver/gpuarchiver-<hex> in the user's cache folder, and never removes it; the sweep
 * removes the ones untouched for SCRATCH_IDLE. Checks, in a folder of its own, that only idle gpuarchiver-* folders go;
 * then, with Metal itself, the two facts the sweep stands on: the folder is where scratch_dir points, and an archive
 * written (and its file) still finds its pipelines once the folder is gone.
 *
 *     clang -O1 -g -Wall -I ../gfx mtl_archive_test.m -framework Metal -framework Foundation -o /tmp/mtl_archive_test && /tmp/mtl_archive_test
 */
#include <sys/time.h>
#include "../gfx/mtl_archive.m"

static int fails;
static void check(int ok, const char *what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what); fails += !ok; }
static int exists(const char *p) { struct stat st; return !stat(p, &st); }
static void put(const char *p) { FILE *f = fopen(p, "w"); if (f) { fputs("x", f); fclose(f); } }
static void age(const char *p, int seconds) { struct timeval t[2]; gettimeofday(&t[0], 0); t[0].tv_sec -= seconds; t[1] = t[0]; utimes(p, t); }
/* a folder laid out as Metal's, every entry's time set back by seconds (the youngest entry's by young, when given) */
static void fake(const char *root, const char *name, int seconds, int young) {
	char p[1400], q[1400];
	snprintf(p, sizeof p, "%s/%s", root, name); mkdir(p, 0755);
	snprintf(q, sizeof q, "%s/air64_v29", p); mkdir(q, 0755);
	snprintf(q, sizeof q, "%s/air64_v29/default-1.metallib", p); put(q); age(q, seconds);
	snprintf(q, sizeof q, "%s/air64_v29", p); age(q, seconds);
	snprintf(q, sizeof q, "%s/PersistentState", p); put(q); age(q, young >= 0 ? young : seconds);
	age(p, seconds);
}
static NSMutableSet *listing(void) {
	NSMutableSet *s = [NSMutableSet set]; DIR *d = opendir(scratch_dir); struct dirent *de;
	while (d && (de = readdir(d))) if (!strncmp(de->d_name, "gpuarchiver-", 12)) [s addObject:@(de->d_name)];
	if (d) closedir(d);
	return s;
}

int main(void) {
	@autoreleasepool {
		/* the sweep, in a folder of its own */
		char root[] = "/tmp/mtl_archive_test.XXXXXX"; if (!mkdtemp(root)) { perror("mkdtemp"); return 1; }
		char p[1400];
		fake(root, "gpuarchiver-idle", SCRATCH_IDLE + 600, -1);
		fake(root, "gpuarchiver-fresh", 0, -1);
		fake(root, "gpuarchiver-in-use", SCRATCH_IDLE + 600, 5);   /* an old folder with a file just written */
		fake(root, "not-metals", SCRATCH_IDLE + 600, -1);
		snprintf(scratch_dir, sizeof scratch_dir, "%s", root);
		scratch_sweep();
		snprintf(p, sizeof p, "%s/gpuarchiver-idle", root); check(!exists(p), "an idle folder is removed");
		snprintf(p, sizeof p, "%s/gpuarchiver-fresh", root); check(exists(p), "a fresh folder stays");
		snprintf(p, sizeof p, "%s/gpuarchiver-in-use", root); check(exists(p), "an old folder with a fresh file stays");
		snprintf(p, sizeof p, "%s/not-metals", root); check(exists(p), "a folder not named gpuarchiver-* stays");
		char rm[1500]; snprintf(rm, sizeof rm, "rm -rf '%s'", root); system(rm);

		/* Metal: where its folder is, and that a written archive doesn't need it */
		dev = MTLCreateSystemDefaultDevice();
		if (!dev) { printf("no Metal device: Metal checks skipped\n"); return fails != 0; }
		char cache[1024]; confstr(_CS_DARWIN_USER_CACHE_DIR, cache, sizeof cache); snprintf(scratch_dir, sizeof scratch_dir, "%s/com.apple.gpuarchiver", cache);
		NSError *e = nil;
		id<MTLLibrary> lib = [dev newLibraryWithSource:@"struct O { float4 p [[position]]; }; vertex O v(uint i [[vertex_id]]) { O o; o.p = float4(i, 0, 0, 1); return o; }"
		                                               @" fragment float4 f() { return float4(0.25, 0.5, 0.75, 1); }" options:nil error:&e];
		MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
		id<MTLFunction> vf = [lib newFunctionWithName:@"v"], ff = [lib newFunctionWithName:@"f"];
		pd.vertexFunction = vf; pd.fragmentFunction = ff; pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
		NSMutableSet *before = listing();
		id<MTLBinaryArchive> a = archive_new(nil);
		check(a && [a addRenderPipelineFunctionsWithDescriptor:pd error:&e], "a pipeline added to an archive");
		NSMutableSet *made = listing(); [made minusSet:before];
		check(made.count == 1, "Metal made its scratch folder where scratch_dir points");
		char file[] = "/tmp/mtl_archive_test.XXXXXX"; close(mkstemp(file));
		check([a serializeToURL:[NSURL fileURLWithPath:@(file)] error:&e], "the archive written");
		for (NSString *n in made) { snprintf(p, sizeof p, "%s/%s", scratch_dir, n.UTF8String); nftw(p, rm_entry, 8, FTW_DEPTH | FTW_PHYS); check(!exists(p), "its scratch folder removed"); }
		pd.binaryArchives = @[a];
		id ps = [dev newRenderPipelineStateWithDescriptor:pd options:MTLPipelineOptionFailOnBinaryArchiveMiss reflection:nil error:nil];
		check(ps != nil, "the archive finds its pipeline without the folder"); [ps release];
		id<MTLBinaryArchive> b = archive_new([NSURL fileURLWithPath:@(file)]);
		pd.binaryArchives = b ? @[b] : nil;
		ps = b ? [dev newRenderPipelineStateWithDescriptor:pd options:MTLPipelineOptionFailOnBinaryArchiveMiss reflection:nil error:nil] : nil;
		check(ps != nil, "so does its file, loaded again"); [ps release];
		NSMutableSet *after = listing(); [after minusSet:before]; [after minusSet:made];
		check(after.count == 0, "loading an archive's file makes no folder");
		unlink(file); [a release]; [b release]; [vf release]; [ff release]; [pd release]; [lib release];
	}
	printf(fails ? "%d failed\n" : "all passed\n", fails);
	return fails != 0;
}
