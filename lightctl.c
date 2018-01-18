/*
 * Build with:
 *
 * gcc -c lightctl.c -o lightctl.o
 * gcc lightctl.o libws2811.a -o lightctl
 */



#include <stdint.h>
#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>

#include "ws2811.h"


ws2811_t ledstrip =
{
    .freq = WS2811_TARGET_FREQ,
    .dmanum = 4,
    .channel =
    {
        [0] =
        {
            .gpionum = 18,
            .count = 167,
            .invert = 0,
            .brightness = 255,
            .strip_type = SK6812_STRIP_GRBW,
        },
        [1] =
        {
            .gpionum = 13,
            .count = 109,
            .invert = 1,
            .brightness = 255,
            .strip_type = SK6812_STRIP_GRBW,
        },
    },
};

// returns a brightness in [0...1]
float get_brightness(uint32_t color) {
    static const uint32_t w_brightness = 5;
    static const uint32_t r_brightness = 2;
    static const uint32_t g_brightness = 3;
    static const uint32_t b_brightness = 2;
    return (float)(((color >> 24) & 0xff) * w_brightness
               + ((color >> 16) & 0xff) * r_brightness
               + ((color >> 8) & 0xff) * g_brightness
               + ((color >> 0) & 0xff) * b_brightness) /
               (float)(w_brightness + r_brightness + g_brightness + b_brightness);
}

static uint32_t limit_brightness(uint32_t color, uint32_t reference_color) {
    float brightness = get_brightness(color);
    float ref_brightness = get_brightness(reference_color);
    if (ref_brightness < brightness) {
        float new_w = (float)((color >> 24) & 0xff) * ref_brightness / brightness;
        float new_r = (float)((color >> 16) & 0xff) * ref_brightness / brightness;
        float new_g = (float)((color >> 8) & 0xff) * ref_brightness / brightness;
        float new_b = (float)((color >> 0) & 0xff) * ref_brightness / brightness;
        return ((uint32_t)(new_w) << 24) + ((uint32_t)(new_r) << 16) +
               ((uint32_t)(new_g) << 8) + ((uint32_t)(new_b) << 0);
    } else {
        return color;
    }
}

static uint32_t blend_colors(uint32_t color1, uint32_t color2, float alpha) {
    float new_w = (1 - alpha) * (float)((color1 >> 24) & 0xff) + alpha * (float)((color2 >> 24) & 0xff);
    float new_r = (1 - alpha) * (float)((color1 >> 16) & 0xff) + alpha * (float)((color2 >> 16) & 0xff);
    float new_g = (1 - alpha) * (float)((color1 >> 8) & 0xff) + alpha * (float)((color2 >> 8) & 0xff);
    float new_b = (1 - alpha) * (float)((color1 >> 0) & 0xff) + alpha * (float)((color2 >> 0) & 0xff);
    return ((uint32_t)(new_w) << 24) + ((uint32_t)(new_r) << 16) +
           ((uint32_t)(new_g) << 8) + ((uint32_t)(new_b) << 0);
}

// It is recommended that you use a tmpfs file, especially if your root file system is on an SD card
static void store_leds(const char *filename, uint32_t *colors, size_t led_count) {
    size_t n_written = 0;
    FILE* file = fopen(filename, "w+");
    if (!file) {
        fprintf(stderr, "failed to save LED state (could not open %s)\n", filename);
    } else {
        n_written += fwrite(colors, sizeof(uint32_t), led_count, file);
        if (n_written < led_count)
            fprintf(stderr, "could not write all data to %s\n", filename);
        if (fclose(file) != 0)
            fprintf(stderr, "could not close %s\n", filename);
    }
}

static void load_leds(const char *filename, uint32_t *colors, size_t led_count) {
    size_t n_read = 0;
    FILE* file = fopen(filename, "r");
    if (!file) {
        fprintf(stderr, "failed to load LED state (could not open %s)\n", filename);
    } else {
        n_read += fread(colors, sizeof(uint32_t), led_count, file);
        if (n_read < led_count)
            fprintf(stderr, "insufficient data in %s\n", filename);
        if (fclose(file) != 0)
            fprintf(stderr, "could not close %s\n", filename);
    }

    // fill undefined LED states with 0
    for (; n_read < led_count; ++n_read)
        colors[n_read] = 0;
}

static float get_timespan(struct timespec *time1, struct timespec *time0) {
    return (float)(time1->tv_sec - time0->tv_sec) + (float)((time1->tv_nsec - time0->tv_nsec) / 1000000ll) / 1e3;
}

static int running = 1;

static void ctrl_c_handler(int signum)
{
	(void)(signum);
    running = 0;
}

static void setup_handlers(void)
{
    struct sigaction sa =
    {
        .sa_handler = ctrl_c_handler,
    };

    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

void print_usage(const char *name) {
    printf("Light Control Utility\n");
    printf("Usage: %s WWRRGGBB [--time t] [--not-brighter]\n", name);
    printf("Sets light color to the hexadecimal color code 0xWWRRGGBB\n");
    printf("  --time t, -t t    Time in seconds for the color fade (defaults to 0 if no value is specified)\n");
    printf("  --not-brighter    if the specified color is brighter than the current color, its brightness is bounded\n");
    printf("\nThe current color is saved to /tmp/leds0 and /tmp/leds1\n");
}


struct params {
    char *prog_name;
    int32_t color;
    bool color_specified;
    float duration;
    bool limit_brightness;
};

static int parse_cmdline(int argc, char *argv[], struct params *params) {
    *params = (struct params){
        .prog_name = *argv,
        .color = 0,
        .color_specified = 0,
        .duration = 0,
        .limit_brightness = 0
    };

    while (++argv, --argc) {
        if (!strcmp(*argv, "--help") || !strcmp(*argv, "-h")) {
            return -1;
        } else if (!strcmp(*argv, "--not-brighter")) {
            params->limit_brightness = 1;
        } else if (!strcmp(*argv, "--time") || !strcmp(*argv, "-t")) {
            if (!(--argc)) {
                fprintf(stderr, "expected argument after --time\n");
                return -1;
            }
            if (sscanf(*(++argv), "%f", &params->duration) < 1) {
                fprintf(stderr, "expected floating point number after --time\n");
                return -1;
            }
        } else if (!params->color_specified) {
            if (sscanf(*argv, "%x", &params->color) < 1) {
                fprintf(stderr, "unknown argument %s (color argument must be a hexadecimal number)\n", *argv);
                return -1;
            }
            params->color_specified = 1;
        } else {
            fprintf(stderr, "unknown argument %s (only one color argument allowed)\n", *argv);
            return -1;
        }
    }
    return 0;
}


int main(int argc, char *argv[])
{
    ws2811_return_t ret;

    struct params params;
    if (parse_cmdline(argc, argv, &params)) {
        print_usage(argv[0]);
        return -1;
    }

    setup_handlers();

    if (setuid(0) != 0) {
        fprintf(stderr, "Could not impersonate root user. Maybe you forgot the following:\n");
        fprintf(stderr, "    sudo chown root:root '%s'\n", argv[0]);
        fprintf(stderr, "    sudo chmod u+s '%s'\n", argv[0]);
        return -1;
    }

    // load start state
    uint32_t start0[ledstrip.channel[0].count];
    uint32_t start1[ledstrip.channel[1].count];
    load_leds("/tmp/leds0", start0, ledstrip.channel[0].count);
    load_leds("/tmp/leds1", start1, ledstrip.channel[1].count);

    // load end state
    uint32_t end0[ledstrip.channel[0].count];
    uint32_t end1[ledstrip.channel[1].count];
    for (size_t i = 0; i < ledstrip.channel[0].count; ++i)
        end0[i] = params.limit_brightness ? limit_brightness(params.color, start0[i]) : params.color;
    for (size_t i = 0; i < ledstrip.channel[1].count; ++i)
        end1[i] = params.limit_brightness ? limit_brightness(params.color, start1[i]) : params.color;

    // load time
    struct timespec starttime, currenttime;
    if (clock_gettime(CLOCK_MONOTONIC, &starttime)) {
        fprintf(stderr, "clock failed\n");
        return -1;
    }

    // init LEDs
    if ((ret = ws2811_init(&ledstrip)) != WS2811_SUCCESS) {
        fprintf(stderr, "ws2811_init failed: %s\n", ws2811_get_return_t_str(ret));
        return ret;
    }
	
    //printf("set color to W: %02x, R: %02x, G: %02x, B: %02x\n", color >> 24, (color >> 16) & 0xff, (color >> 8) & 0xff, (color >> 0) & 0xff);

    while (running) {
        if (clock_gettime(CLOCK_MONOTONIC, &currenttime)) {
            fprintf(stderr, "clock failed\n");
            break;
        }

        float progress = get_timespan(&currenttime, &starttime) / params.duration;
        if (!(progress < 1)) // also evaluates to true for inf and nan
            progress = 1;
        //printf("time: %f, progress: %f\n", get_timespan(&currenttime, &starttime), progress);

        // render LEDs based on progress
        for (size_t i = 0; i < ledstrip.channel[0].count; ++i)
            ledstrip.channel[0].leds[i] = blend_colors(start0[i], end0[i], progress);
        for (size_t i = 0; i < ledstrip.channel[1].count; ++i)
            ledstrip.channel[1].leds[i] = blend_colors(start1[i], end1[i], progress);

        if ((ret = ws2811_render(&ledstrip)) != WS2811_SUCCESS) {
            fprintf(stderr, "ws2811_render failed: %s\n", ws2811_get_return_t_str(ret));
            break;
        }

        store_leds("/tmp/leds0", ledstrip.channel[0].leds, ledstrip.channel[0].count);
        store_leds("/tmp/leds1", ledstrip.channel[1].leds, ledstrip.channel[1].count);

        if (progress >= 1)
            break;

        // 100 frames /sec
        usleep(1000000 / 100);
    };


    ws2811_fini(&ledstrip);

    return ret;
}


