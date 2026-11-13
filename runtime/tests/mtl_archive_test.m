/*
 * runtime/gfx/mtl_archive.m's handling of Metal's scratch folders (macOS). Metal gives every binary archive a pipeline
 * is added to a folder, com.apple.gpuarchiver/gpuarchiver-<hex> in a cache shared by every program, but exposes no
 * ownership information. Checks that opening and writing Nativ's archive leaves an old unowned folder alone. Then it
 * removes the directory observed during this test's own archive operation and checks that the archive and its file
 * still find their pipelines.
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
static void fake(const char *p, int seconds) {
	char q[1400];
	snprintf(q, sizeof q, "%s/air64_v29", p); mkdir(q, 0755);
	snprintf(q, sizeof q, "%s/air64_v29/default-1.metallib", p); put(q); age(q, seconds);
	snprintf(q, sizeof q, "%s/air64_v29", p); age(q, seconds);
	snprintf(q, sizeof q, "%s/PersistentState", p); put(q); age(q, seconds);
	age(p, seconds);
}
static NSMutableSet *listing(const char *root) {
	NSMutableSet *s = [NSMutableSet set]; DIR *d = opendir(root); struct dirent *de;
	while (d && (de = readdir(d))) if (!strncmp(de->d_name, "gpuarchiver-", 12)) [s addObject:@(de->d_name)];
	if (d) closedir(d);
	return s;
}
static void air_store_test(uint64_t key, int frag, const char *entry, const void *air, size_t n) {
	(void)key; (void)frag; (void)entry; (void)air; (void)n;
}

int main(void) {
	@autoreleasepool {
		dev = MTLCreateSystemDefaultDevice();
		if (!dev) { printf("no Metal device: Metal checks skipped\n"); return fails != 0; }
		char cache[1024], scratch[1200], unowned[1400], p[1600];
		confstr(_CS_DARWIN_USER_CACHE_DIR, cache, sizeof cache); snprintf(scratch, sizeof scratch, "%s/com.apple.gpuarchiver", cache); mkdir(scratch, 0755);
		snprintf(unowned, sizeof unowned, "%s/gpuarchiver-nativ-unowned-test.XXXXXX", scratch);
		if (!mkdtemp(unowned)) { perror("mkdtemp"); return 1; }
		fake(unowned, 24 * 60 * 60);
		char root[] = "/tmp/mtl_archive_test.XXXXXX"; if (!mkdtemp(root)) { perror("mkdtemp"); return 1; }
		check(mtl_archive_open(root, air_store_test), "Nativ's archive opens");
		check(exists(unowned), "an old unowned scratch folder stays after archive startup");

		NSError *e = nil;
		id<MTLLibrary> lib = [dev newLibraryWithSource:@"struct O { float4 p [[position]]; }; vertex O v(uint i [[vertex_id]]) { O o; o.p = float4(i, 0, 0, 1); return o; }"
		                                               @" fragment float4 f() { return float4(0.25, 0.5, 0.75, 1); }" options:nil error:&e];
		MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
		id<MTLFunction> vf = [lib newFunctionWithName:@"v"], ff = [lib newFunctionWithName:@"f"];
		pd.vertexFunction = vf; pd.fragmentFunction = ff; pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
		NSMutableSet *before = listing(scratch);
		id<MTLBinaryArchive> a = archive_new(nil);
		check(a && [a addRenderPipelineFunctionsWithDescriptor:pd error:&e], "a pipeline added to an archive");
		NSMutableSet *made = listing(scratch); [made minusSet:before];
		check(made.count == 1, "Metal made one scratch folder for this archive");
		char file[] = "/tmp/mtl_archive_test.XXXXXX"; close(mkstemp(file));
		check([a serializeToURL:[NSURL fileURLWithPath:@(file)] error:&e], "the archive written");
		if (made.count == 1) {
			NSString *n = made.anyObject; snprintf(p, sizeof p, "%s/%s", scratch, n.UTF8String);
			[[NSFileManager defaultManager] removeItemAtPath:@(p) error:nil];
			check(!exists(p), "this test's completed scratch folder can be removed");
		}
		check(exists(unowned), "the old unowned scratch folder still stays");
		pd.binaryArchives = @[a];
		id ps = [dev newRenderPipelineStateWithDescriptor:pd options:MTLPipelineOptionFailOnBinaryArchiveMiss reflection:nil error:nil];
		check(ps != nil, "the archive finds its pipeline without the folder"); [ps release];
		id<MTLBinaryArchive> b = archive_new([NSURL fileURLWithPath:@(file)]);
		pd.binaryArchives = b ? @[b] : nil;
		ps = b ? [dev newRenderPipelineStateWithDescriptor:pd options:MTLPipelineOptionFailOnBinaryArchiveMiss reflection:nil error:nil] : nil;
		check(ps != nil, "so does its file, loaded again"); [ps release];
		NSMutableSet *after = listing(scratch); [after minusSet:before]; [after minusSet:made];
		check(after.count == 0, "loading an archive's file makes no folder");
		unlink(file); [a release]; [b release]; [vf release]; [ff release]; [pd release]; [lib release];
		[[NSFileManager defaultManager] removeItemAtPath:@(unowned) error:nil];
		[[NSFileManager defaultManager] removeItemAtPath:@(root) error:nil];
	}
	printf(fails ? "%d failed\n" : "all passed\n", fails);
	return fails != 0;
}
