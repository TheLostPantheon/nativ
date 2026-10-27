/*
 * Motion: a gamepad's gyroscope and accelerometer (DS4/DualSense through SDL3) as the Vita's motion
 * sensor -- what sceMotionGetState reports (display.c). window.c feeds every SDL sensor sample here;
 * the game's thread reads the result. Uncharted (aiming, balance, puzzles) and Killzone (aim
 * compensation) read it; the other titles never ask.
 *
 * Frames. SDL gives a gamepad's samples in its own axes, the same however it's held: +x right, +y up
 * out of the face, +z toward the player; accel in m/s^2 as the specific force (at rest: +9.8 along
 * "up"), gyro in rad/s, counter-clockwise positive. The Vita's are its device axes: +x right, +y up
 * the screen, +z out of the screen; acceleration in g as the gravity vector (lying flat, screen up:
 * (0, 0, -1)), angular velocity in rad/s. Which Vita pose a pad in the hand stands for is the
 * "gyro_pose" setting:
 *   upright (default): a pad held level is a Vita held up in front of you, screen facing you -- the
 *     axes line up one to one, so turning the pad left/right turns the "Vita" about its own vertical
 *     axis, as turning a Vita in your hands does;
 *   flat: a pad held level is a Vita lying flat, screen up (the pad's +y is the Vita's +z, its +z
 *     the Vita's -y).
 * Both are proper rotations, so every motion of the pad is the same motion of the Vita; they differ
 * by a fixed quarter turn in the resting pose.
 *
 * Orientation (deviceQuat, the rotation matrices) is a Mahony filter: the gyro integrated, nudged
 * toward the accelerometer's gravity while it measures about 1 g (moving hard, it doesn't), so tilt
 * stays true and only the heading (yaw) can drift -- recentred on request (R3; sceMotionReset; the
 * menu), which turns the current heading into straight ahead. The quaternion takes the device frame
 * to a world frame with +z up (identity = lying flat, screen up, as the Vita reports at rest).
 */
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include "settings.h"

static pthread_mutex_t mx = PTHREAD_MUTEX_INITIALIZER;
static float acc_v[3] = {0, 0, -1}, gyr_v[3];      /* the latest samples, Vita frame (g, rad/s) */
static float q[4] = {1, 0, 0, 0};                  /* w, x, y, z: device -> world */
static float yaw_off[4] = {1, 0, 0, 0};            /* the recentre: a turn about world z applied on output */
static uint64_t last_gyro_ns, last_any_ns;
static int have_acc, started, want_recenter;
static uint64_t mono_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }

static int pose_flat(void) { char b[16]; return !strcmp(settings_str("gyro_pose", "upright", b, sizeof b), "flat"); }
/* SDL gamepad axes -> Vita device axes, per the pose (see the header) */
static int virt;           /* the current samples are the virtual pad's (motion_feed_virtual): always the upright pose */
void motion_pause(void);
static void to_vita(const float s[3], float v[3]) {
	if (!virt && pose_flat()) { v[0] = s[0]; v[1] = -s[2]; v[2] = s[1]; }
	else { v[0] = s[0]; v[1] = s[1]; v[2] = s[2]; }
}
static float clampf(float v, float lim) { return v < -lim ? -lim : v > lim ? lim : v; }
static void qmul(const float a[4], const float b[4], float r[4]) {
	float w = a[0]*b[0] - a[1]*b[1] - a[2]*b[2] - a[3]*b[3];
	float x = a[0]*b[1] + a[1]*b[0] + a[2]*b[3] - a[3]*b[2];
	float y = a[0]*b[2] - a[1]*b[3] + a[2]*b[0] + a[3]*b[1];
	float z = a[0]*b[3] + a[1]*b[2] - a[2]*b[1] + a[3]*b[0];
	r[0] = w; r[1] = x; r[2] = y; r[3] = z;
}
static void qnorm(float a[4]) { float n = sqrtf(a[0]*a[0] + a[1]*a[1] + a[2]*a[2] + a[3]*a[3]); if (n > 0) for (int i = 0; i < 4; i++) a[i] /= n; }
/* R (device -> world) times a device vector */
static void qrot(const float a[4], const float v[3], float r[3]) {
	float w = a[0], x = a[1], y = a[2], z = a[3];
	r[0] = (1 - 2*(y*y + z*z)) * v[0] + 2*(x*y - w*z) * v[1] + 2*(x*z + w*y) * v[2];
	r[1] = 2*(x*y + w*z) * v[0] + (1 - 2*(x*x + z*z)) * v[1] + 2*(y*z - w*x) * v[2];
	r[2] = 2*(x*z - w*y) * v[0] + 2*(y*z + w*x) * v[1] + (1 - 2*(x*x + y*y)) * v[2];
}
/* the orientation from gravity alone (heading arbitrary): the shortest turn taking the measured "up"
 * (device frame) onto world +z */
static void q_from_up(const float up[3], float r[4]) {
	float n = sqrtf(up[0]*up[0] + up[1]*up[1] + up[2]*up[2]); if (n < 1e-6f) { r[0] = 1; r[1] = r[2] = r[3] = 0; return; }
	float u[3] = {up[0]/n, up[1]/n, up[2]/n};
	/* axis = u x z, angle = acos(u.z) */
	float ax = u[1], ay = -u[0], d = u[2];
	if (d < -0.9999f) { r[0] = 0; r[1] = 1; r[2] = r[3] = 0; return; }   /* upside down: half a turn about x */
	r[0] = 1 + d; r[1] = ax; r[2] = ay; r[3] = 0; qnorm(r);
}
/* the heading to undo: the direction a device axis points in the world's horizontal plane -- +y (up
 * the screen) when it lies near level, else -z (out of the back, straight ahead when held up) */
static void make_recenter(void) {
	float y_axis[3] = {0, 1, 0}, back[3] = {0, 0, -1}, h[3];
	qrot(q, y_axis, h);
	if (h[0]*h[0] + h[1]*h[1] < 0.25f) qrot(q, back, h);
	float yaw = atan2f(h[0], h[1]);   /* from world +y, clockwise seen from above */
	yaw_off[0] = cosf(yaw * 0.5f); yaw_off[1] = yaw_off[2] = 0; yaw_off[3] = sinf(yaw * 0.5f);   /* turn by +yaw about z: brings h back onto +y */
}

/* The accelerometer is low-passed (first order, ACC_TAU): a pad in the hand reports every jolt of a
 * button press raw, a few hundred times a second, where a Vita's sensor is far smoother. Uncharted's
 * "shake" gesture fires on two frame-to-frame jumps of |a|^2 over 1 g^2 -- ever, its strike count only
 * clears when it fires -- so button presses on a held pad shook it, more often at 60 fps (twice the
 * frames to catch a spike on), and a shake can dismiss a touch prompt (the machete minigame's, 2026-10-12:
 * "if I place the controller flat for the press it works"). 40 ms keeps tilts and a real shake. */
#define ACC_TAU 0.040
static uint64_t acc_t_ns;
static int use_virt = -1;  /* the source window.c chose (motion_use_virtual): 1 the virtual pad, 0 a real pad's sensors, -1 not yet */
static float xf_acc[3], xf_q[4]; static uint64_t xf_ns;   /* a source change's cross-fade: what the game read at it (w, x, y, z), and when */
static void feed_accel(const float s[3], uint64_t t_ns) {
	float v[3]; to_vita(s, v);
	pthread_mutex_lock(&mx);
	double dt = acc_t_ns && t_ns > acc_t_ns ? (t_ns - acc_t_ns) / 1e9 : 0;
	float k = !have_acc || !acc_t_ns || dt > 0.25 ? 1.0f : (float)(1.0 - exp(-dt / ACC_TAU));   /* (the first sample, or after a gap: as it is) */
	for (int i = 0; i < 3; i++) { float raw = clampf(-v[i] / 9.80665f, 7.0f); acc_v[i] += k * (raw - acc_v[i]); }   /* gravity, in g */
	acc_t_ns = t_ns; have_acc = 1; last_any_ns = mono_ns();
	pthread_mutex_unlock(&mx);
}
static void feed_gyro(const float s[3], uint64_t t_ns, double sens) {
	float v[3]; to_vita(s, v);
	pthread_mutex_lock(&mx);
	for (int i = 0; i < 3; i++) gyr_v[i] = clampf((float)(v[i] * sens), 31.4159f);
	last_any_ns = mono_ns();
	if (!have_acc) { pthread_mutex_unlock(&mx); return; }
	float up[3] = {-acc_v[0], -acc_v[1], -acc_v[2]};   /* the accelerometer's "up" */
	if (!started) { q_from_up(up, q); started = 1; want_recenter = 1; last_gyro_ns = t_ns; }
	double dt = last_gyro_ns && t_ns > last_gyro_ns ? (t_ns - last_gyro_ns) / 1e9 : 0;
	last_gyro_ns = t_ns;
	if (dt > 0.05) dt = 0.05;   /* a gap (the menu was open, a dropped report): no huge step */
	if (dt > 0) {
		float g[3] = {gyr_v[0], gyr_v[1], gyr_v[2]};
		float n = sqrtf(up[0]*up[0] + up[1]*up[1] + up[2]*up[2]);
		if (n > 0.75f && n < 1.25f) {   /* tilt correction only while the accelerometer reads about 1 g */
			float a[3] = {up[0]/n, up[1]/n, up[2]/n};
			/* world up in device coordinates: the third row of R */
			float w = q[0], x = q[1], y = q[2], z = q[3];
			float vx = 2*(x*z - w*y), vy = 2*(y*z + w*x), vz = 1 - 2*(x*x + y*y);
			float ex = a[1]*vz - a[2]*vy, ey = a[2]*vx - a[0]*vz, ez = a[0]*vy - a[1]*vx;
			const float kp = 1.0f;   /* light: the games smooth the motion themselves (Uncharted turns the SDK's own filters off) */
			g[0] += kp*ex; g[1] += kp*ey; g[2] += kp*ez;
		}
		float dq[4] = {0, g[0], g[1], g[2]}, qd[4];
		qmul(q, dq, qd);
		for (int i = 0; i < 4; i++) q[i] += 0.5f * qd[i] * (float)dt;
		qnorm(q);
	}
	if (want_recenter) { make_recenter(); want_recenter = 0; }
	pthread_mutex_unlock(&mx);
}
void motion_feed_accel(const float s[3], uint64_t t_ns) { if (use_virt != 1) feed_accel(s, t_ns); }   /* SDL accel, m/s^2; the sample's own timestamp (dropped while the virtual pad drives) */
void motion_feed_gyro(const float s[3], uint64_t t_ns) { if (use_virt != 1) feed_gyro(s, t_ns, settings_float("gyro_sens", 1.0)); }   /* SDL gyro, rad/s */
/* Which source drives the sensor -- window.c's choice every frame, from the input scheme in use: the virtual pad
 * (keyboard + mouse, or a pad without a gyro) or a real pad's gyro; the other's samples are dropped. (By whichever
 * was sending, a DualSense lying on the desk would hold the mouse's tilt off.) A change starts the new source's
 * filter afresh and cross-fades what the game reads over 0.25 s from where the old one was -- switching mid-beam
 * is a short glide, not a lurch -- and returns 1 with *roll the roll the game was reading, which the virtual pad
 * starts from. */
int motion_use_virtual(int on, float *roll) {
	pthread_mutex_lock(&mx);
	if (on == use_virt) { pthread_mutex_unlock(&mx); return 0; }
	int had = use_virt >= 0 && started && last_any_ns && mono_ns() - last_any_ns < 500000000ull;
	if (had) { qmul(yaw_off, q, xf_q); memcpy(xf_acc, acc_v, 12); xf_ns = mono_ns(); } else xf_ns = 0;
	*roll = had ? atan2f(acc_v[0], -acc_v[1]) : 0;   /* (upright: a roll r reads as gravity (sin r, -cos r, .)) */
	use_virt = on; virt = on;
	started = have_acc = 0; acc_t_ns = 0; last_gyro_ns = 0;
	pthread_mutex_unlock(&mx);
	return 1;
}
/* The virtual pad. Keyboard and mouse have no gyro, and Uncharted's balance beams and river slide are steered by
 * tilting the Vita: window.c turns mouse (and Z/X) into a tilt angle, and this sends what a pad held level ("upright":
 * a Vita held up facing you) and rolled by that angle like a steering wheel would -- the accelerometer's specific
 * force and the roll rate -- through the same filter as a real pad's, so the game reads one consistent device (its
 * gravity vector, rates, quaternion and matrices agree). angle: radians, positive = the right side down. Called
 * every window frame; nothing unless motion_use_virtual chose it. */
void motion_feed_virtual(float angle, uint64_t t_ns) {
	static float prev; static uint64_t prev_ns;
	if (use_virt != 1) { prev_ns = 0; return; }
	float rate = prev_ns && t_ns > prev_ns ? (angle - prev) / (float)((t_ns - prev_ns) / 1e9) : 0;
	prev = angle; prev_ns = t_ns;
	float acc[3] = { -9.80665f * sinf(angle), 9.80665f * cosf(angle), 0 }, gyr[3] = { 0, 0, -rate };   /* (pad axes: +y up, +z toward the player; the right side down is a clockwise roll as you see it, negative about +z) */
	feed_accel(acc, t_ns); feed_gyro(gyr, t_ns, 1.0);
}
void motion_recenter(void) { pthread_mutex_lock(&mx); if (started) make_recenter(); else want_recenter = 1; pthread_mutex_unlock(&mx); }
/* the menu opened or the pad went away: the next sample starts a fresh interval */
void motion_pause(void) { pthread_mutex_lock(&mx); last_gyro_ns = 0; pthread_mutex_unlock(&mx); }
void motion_lost(void) { pthread_mutex_lock(&mx); started = have_acc = 0; last_any_ns = 0; acc_t_ns = 0; pthread_mutex_unlock(&mx); }
/* live: a pad has sent samples within the last half second (and "gyro" isn't off). Out: acceleration
 * (g), angular velocity (rad/s), orientation x, y, z, w -- the Vita's own layout and units */
int motion_live(float acc[3], float gyr[3], float quat[4]) {
	if (!virt && !settings_int("gyro", 1)) return 0;   /* ("gyro" 0 turns a pad's off; the virtual pad has its own way in: window.c's tilt) */
	pthread_mutex_lock(&mx);
	int live = started && last_any_ns && mono_ns() - last_any_ns < 500000000ull;
	if (live) {
		float o[4]; qmul(yaw_off, q, o);
		memcpy(acc, acc_v, 12); memcpy(gyr, gyr_v, 12);
		if (xf_ns) {   /* a source change's cross-fade (motion_use_virtual) */
			float w = (float)((mono_ns() - xf_ns) / 250e6);
			if (w >= 1) xf_ns = 0;
			else {
				float d = xf_q[0]*o[0] + xf_q[1]*o[1] + xf_q[2]*o[2] + xf_q[3]*o[3], sgn = d < 0 ? -1.0f : 1.0f;
				for (int i = 0; i < 4; i++) o[i] = sgn * xf_q[i] * (1 - w) + o[i] * w;
				qnorm(o);
				for (int i = 0; i < 3; i++) acc[i] = xf_acc[i] * (1 - w) + acc[i] * w;
			}
		}
		quat[0] = o[1]; quat[1] = o[2]; quat[2] = o[3]; quat[3] = o[0];
	}
	pthread_mutex_unlock(&mx);
	return live;
}
/* the rotation matrix of a quaternion (x, y, z, w), rows as SceFMatrix4 stores them (x, y, z, w rows
 * of four floats, the last row and column the identity's) */
void motion_matrix(const float quat[4], float m[16]) {
	float a[4] = {quat[3], quat[0], quat[1], quat[2]}, c[3], e[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
	memset(m, 0, 64);
	for (int col = 0; col < 3; col++) { qrot(a, e[col], c); for (int row = 0; row < 3; row++) m[row * 4 + col] = c[row]; }
	m[15] = 1;
}
