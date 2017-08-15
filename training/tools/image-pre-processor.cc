/*
 * Copyright (C) 2017 Kwamecorp
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <sys/types.h>
#include <sys/stat.h>

#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <assert.h>
#include <dirent.h>
#include <stdint.h>
#include <libgen.h>
#include <assert.h>
#include <pthread.h>
#include <getopt.h>

#include <type_traits>
#include <queue>
#include <random>

#include <ImfInputFile.h>
#include <ImfOutputFile.h>
#include <ImfStringAttribute.h>
#include <ImfMatrixAttribute.h>
#include <ImfArray.h>
#include <ImfChannelList.h>

#include <ImathBox.h>

#ifdef DEBUG
#define PNG_DEBUG 3
#define debug(ARGS...) printf(ARGS)
#else
#define debug(ARGS...) do {} while(0)
#endif

#include <png.h>

#define ARRAY_LEN(X) (sizeof(X)/sizeof(X[0]))

#define BACKGROUND_ID 33

enum image_format {
    IMAGE_FORMAT_X8,
    IMAGE_FORMAT_XFLOAT,
};

struct image
{
    enum image_format format;
    int width;
    int height;
    int stride;

    union {
        uint8_t *data_u8;
        float *data_float;
    };
};


/* Work is grouped by directories where the clothes are the same since we want
 * to diff sequential images to discard redundant frames which makes sense
 * for a single worker thread to handle
 */
struct work {
    char *dir;
    std::vector<char *> files;
};

struct worker_state
{
    int idx;
    pthread_t thread;
};

using namespace OPENEXR_IMF_NAMESPACE;
using namespace IMATH_NAMESPACE;

static const char *top_src_dir;
static const char *top_out_dir;

static bool write_half_float = true;

static int labels_width = 0;
static int labels_height = 0;

static std::vector<struct worker_state> workers;

static pthread_mutex_t work_queue_lock = PTHREAD_MUTEX_INITIALIZER;
static std::queue<struct work> work_queue;

static int indent = 0;

static int grey_to_id_map[255];

#define MAX_PACKED_INDEX 33
static int left_to_right_map[MAX_PACKED_INDEX + 1];

static pthread_once_t cpu_count_once = PTHREAD_ONCE_INIT;
static int n_cpus = 0;

static std::default_random_engine rand_generator;

static png_color palette[] = {
    { 0xff, 0x5d, 0xaa },
    { 0xd1, 0x15, 0x40 },
    { 0xda, 0x1d, 0x0e },
    { 0xdd, 0x5d, 0x1e },
    { 0x49, 0xa2, 0x24 },
    { 0x29, 0xdc, 0xe3 },
    { 0x02, 0x68, 0xc2 },
    { 0x90, 0x29, 0xf9 },
    { 0xff, 0x00, 0xcf },
    { 0xef, 0xd2, 0x37 },
    { 0x92, 0xa1, 0x3a },
    { 0x48, 0x21, 0xeb },
    { 0x2f, 0x93, 0xe5 },
    { 0x1d, 0x6b, 0x0e },
    { 0x07, 0x66, 0x4b },
    { 0xfc, 0xaa, 0x98 },
    { 0xb6, 0x85, 0x91 },
    { 0xab, 0xae, 0xf1 },
    { 0x5c, 0x62, 0xe0 },
    { 0x48, 0xf7, 0x36 },
    { 0xa3, 0x63, 0x0d },
    { 0x78, 0x1d, 0x07 },
    { 0x5e, 0x3c, 0x00 },
    { 0x9f, 0x9f, 0x60 },
    { 0x51, 0x76, 0x44 },
    { 0xd4, 0x6d, 0x46 },
    { 0xff, 0xfb, 0x7e },
    { 0xd8, 0x4b, 0x4b },
    { 0xa9, 0x02, 0x52 },
    { 0x0f, 0xc1, 0x66 },
    { 0x2b, 0x5e, 0x44 },
    { 0x00, 0x9c, 0xad },
    { 0x00, 0x40, 0xad },
    { 0x21, 0x21, 0x21 },
};

#define xsnprintf(dest, fmt, ...) do { \
        if (snprintf(dest, sizeof(dest), fmt,  __VA_ARGS__) >= (int)sizeof(dest)) \
            exit(1); \
    } while(0)

static void *
xmalloc(size_t size)
{
    void *ret = malloc(size);
    if (ret == NULL)
        exit(1);
    return ret;
}

static uint64_t
get_time(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);

    return ((uint64_t)ts.tv_sec) * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

const char *
get_duration_ns_print_scale_suffix(uint64_t duration_ns)
{
    if (duration_ns > 1000000000)
        return "s";
    else if (duration_ns > 1000000)
        return "ms";
    else if (duration_ns > 1000)
        return "us";
    else
        return "ns";
}

float
get_duration_ns_print_scale(uint64_t duration_ns)
{
    if (duration_ns > 1000000000)
        return duration_ns / 1e9;
    else if (duration_ns > 1000000)
        return duration_ns / 1e6;
    else if (duration_ns > 1000)
        return duration_ns / 1e3;
    else
        return duration_ns;
}

static struct image *
xalloc_image(enum image_format format,
             int width,
             int height)
{
    struct image *img = (struct image *)xmalloc(sizeof(struct image));
    img->format = format;
    img->width = width;
    img->height = height;

    switch (format) {
    case IMAGE_FORMAT_X8:
        img->stride = width;
        break;
    case IMAGE_FORMAT_XFLOAT:
        img->stride = width * sizeof(float);
        break;
    }
    img->data_u8 = (uint8_t *)xmalloc(img->stride * img->height);

    return img;
}

static void
free_image(struct image *image)
{
    free(image->data_u8);
    free(image);
}

static bool
write_png_file(const char *filename,
                int width, int height,
                png_bytep *row_pointers,
                png_byte color_type,
                png_byte bit_depth)
{
    png_structp png_ptr;
    png_infop info_ptr;
    bool ret = false;

    /* create file */
    FILE *fp = fopen(filename, "wb");
    if (!fp) {
        fprintf(stderr, "Failed to open %s for writing\n", filename);
        return false;
    }

    png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png_ptr) {
        fprintf(stderr, "png_create_write_struct faile\nd");
        goto error_create_write;
    }

    info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr) {
        fprintf(stderr, "png_create_info_struct failed");
        goto error_create_info;
    }

    if (setjmp(png_jmpbuf(png_ptr))) {
        fprintf(stderr, "PNG write failure");
        goto error_write;
    }

    png_init_io(png_ptr, fp);

    png_set_IHDR(png_ptr, info_ptr, width, height,
                 bit_depth, color_type, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);

    if (color_type == PNG_COLOR_TYPE_PALETTE)
        png_set_PLTE(png_ptr, info_ptr, palette, ARRAY_LEN(palette));

    png_write_info(png_ptr, info_ptr);

    png_write_image(png_ptr, row_pointers);

    png_write_end(png_ptr, NULL);

    ret = true;

error_write:
    png_destroy_info_struct(png_ptr, &info_ptr);
error_create_info:
    png_destroy_write_struct(&png_ptr, NULL);
error_create_write:
    fclose(fp);

    return ret;
}


/* Using EXR is a nightmare. If we try and only add an 'R' channel then
 * e.g. Krita will be able to open the file and it looks reasonable,
 * but OpenCV will end up creating an image with the G and B containing
 * uninitialized garbage. If instead we create a 'Y' only image then
 * OpenCV has special handling for that case and loads it as a greyscale
 * image but Krita will bork and warn that it's not supported. We choose
 * the version that works with OpenCV...
 */
static void
write_exr(const char *filename,
          struct image *image)
{
    int width = image->width;
    int height = image->height;
    Header header(width, height);

    if (write_half_float) {
        Array2D<half> half_image;

        half_image.resizeErase(height, width);

        for (int y = 0; y < height; y++) {
            float *float_row = image->data_float + width * y;
            half *half_row = &half_image[y][0];

            for (int x = 0; x < width; x++)
                half_row[x] = float_row[x];
        }

        header.channels().insert("Y", Channel(HALF));

        OutputFile out_file(filename, header);

        FrameBuffer outFrameBuffer;
        outFrameBuffer.insert("Y",
                              Slice(HALF,
                                    (char *)&half_image[0][0],
                                    sizeof(half_image[0][0]), // x stride,
                                    sizeof(half_image[0][0]) * width)); // y stride

        out_file.setFrameBuffer(outFrameBuffer);
        out_file.writePixels(height);
    } else {
        header.channels().insert("Y", Channel(FLOAT));

        OutputFile out_file(filename, header);

        FrameBuffer outFrameBuffer;
        outFrameBuffer.insert("Y",
                              Slice(FLOAT,
                                    (char *)image->data_float,
                                    sizeof(float), // x stride,
                                    sizeof(float) * width)); // y stride

        out_file.setFrameBuffer(outFrameBuffer);
        out_file.writePixels(height);
    }
}


static struct image *
load_frame_labels(const char *dir,
                  const char *filename)
{
    char input_filename[1024];

    FILE *fp;

    unsigned char header[8]; // 8 is the maximum size that can be checked
    png_structp png_ptr;
    png_infop info_ptr;

    int width, height;

    png_bytep *rows;

    int row_stride;

    struct image *img = NULL, *ret = NULL;

    xsnprintf(input_filename, "%s/labels/%s/%s", top_src_dir, dir, filename);

    /* open file and test for it being a png */
    fp = fopen(input_filename, "rb");
    if (!fp) {
        fprintf(stderr, "Failed to open %s for reading\n", input_filename);
        goto error_open;
    }

    if (fread(header, 1, 8, fp) != 8) {
        fprintf(stderr, "IO error reading %s file\n", input_filename);
        goto error_check_header;
    }
    if (png_sig_cmp(header, 0, 8)) {
        fprintf(stderr, "%s was not recognised as a PNG file\n", input_filename);
        goto error_check_header;
    }

    /* initialize stuff */
    png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png_ptr) {
        fprintf(stderr, "png_create_read_struct failed\n");
        goto error_create_read;
    }

    info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr) {
        fprintf(stderr, "png_create_info_struct failed\n");
        goto error_create_info;
    }

    if (setjmp(png_jmpbuf(png_ptr))) {
        fprintf(stderr, "libpng setjmp failure\n");
        goto error_png_setjmp;
    }

    png_init_io(png_ptr, fp);
    png_set_sig_bytes(png_ptr, 8);

    png_read_info(png_ptr, info_ptr);

    width = png_get_image_width(png_ptr, info_ptr);
    height = png_get_image_height(png_ptr, info_ptr);

    if (labels_width) {
        if (width != labels_width || height != labels_height) {
            fprintf(stderr, "Inconsistent size for %s (%dx%d) of label image (expected %dx%d)\n",
                    input_filename, width, height, labels_width, labels_height);
            exit(1);
        }
    }

    png_read_update_info(png_ptr, info_ptr);

    row_stride = png_get_rowbytes(png_ptr, info_ptr);

    img = xalloc_image(IMAGE_FORMAT_X8, width, height);
    rows = (png_bytep *)alloca(sizeof(png_bytep) * height);

    for (int y = 0; y < height; y++)
        rows[y] = (png_byte *)(img->data_u8 + row_stride * y);

    if (setjmp(png_jmpbuf(png_ptr))) {
        fprintf(stderr, "png_read_image_error\n");
        goto error_read_image;
    }

    png_read_image(png_ptr, rows);
    debug("read %s/%s (%dx%d) OK\n", dir, filename, width, height);

    for (int y = 0; y < height; y++) {
        uint8_t *row = img->data_u8 + row_stride * y;

        for (int x = 0; x < width; x++) {
            row[x] = grey_to_id_map[row[x]];

            if (row[x] > MAX_PACKED_INDEX) {
                fprintf(stderr, "Failed to map a label value of 0x%x/%d in image %s\n",
                        row[x], row[x],
                        input_filename);
                goto error_read_image;
            }
        }
    }

    ret = img;

error_read_image:
    if (img && ret == NULL)
        free_image(img);
error_png_setjmp:
    png_destroy_info_struct(png_ptr, &info_ptr);
error_create_info:
    png_destroy_read_struct(&png_ptr, NULL, NULL);
error_create_read:

error_check_header:
    fclose(fp);

error_open:

    return ret;
}


static void
flip_frame_depth(struct image *__restrict__ depth,
                 struct image *__restrict__ out)
{
    int width = depth->width;
    int height = depth->height;

    for (int y = 0; y < height; y++) {
        float *depth_row = depth->data_float + y * width;
        float *out_row = out->data_float + y * width;

        for (int x = 0; x < width; x++) {
            int opposite = width - 1 - x;

            out_row[x] = depth_row[opposite];
            out_row[opposite] = depth_row[x];
        }
    }
}

static void
flip_frame_labels(struct image *__restrict__ labels,
                  struct image *__restrict__ out)
{
    int width = labels->width;
    int height = labels->height;

    for (int y = 0; y < height; y++) {
        uint8_t *label_row = labels->data_u8 + y * width;
        uint8_t *out_row = out->data_u8 + y * width;

        for (int x = 0; x < width; x++) {
            int opposite = width - 1 - x;

            out_row[x] = label_row[opposite];
            out_row[opposite] = label_row[x];
        }
    }
}

static bool
frame_diff(struct image *a, struct image *b,
           int *n_different_px_out,
           int *n_body_px_out)
{
    int width = a->width;
    int height = a->height;
    int n_body_px = 0;
    int n_different_px = 0;

    for (int y = 0; y < height; y++) {
        uint8_t *row = a->data_u8 + a->stride * y;

        for (int x = 0; x < width; x++) {
            if (row[x] != BACKGROUND_ID)
                n_body_px++;
        }
    }

    for (int y = 0; y < height; y++) {
        uint8_t *a_row = a->data_u8 + a->stride * y;
        uint8_t *b_row = b->data_u8 + b->stride * y;

        for (int x = 0; x < width; x++) {
            if (a_row[x] != b_row[x])
                n_different_px++;
        }
    }

    *n_different_px_out = n_different_px;
    *n_body_px_out = n_body_px;

    if (n_different_px <= (n_body_px / 1000))
        return false;
    else
        return true;
}

static void
frame_add_noise(const struct image *__restrict__ labels,
                const struct image *__restrict__ depth,
                struct image *__restrict__ noisy_labels,
                struct image *__restrict__ noisy_depth)
{
    int width = labels->width;
    int height = labels->height;
    const float *in_depth_px = depth->data_float;
    const uint8_t *in_labels_px = labels->data_u8;
    float *out_depth_px = noisy_depth->data_float;
    uint8_t *out_labels_px = noisy_labels->data_u8;

    rand_generator.seed(234987);

    /* For picking one of 8 random neighbours for fuzzing the silhouettes */
    std::uniform_int_distribution<int> uniform_distribution(0, 7);

    /* We use a Gaussian distribution of error offsets for the depth values.
     *
     * We want the variance to mostly be ~ +- 2mm.
     *
     * According to Wikipedia the full width at tenth of maximum of a Gaussian
     * curve = approximately 4.29193c (where c is the standard deviation which
     * we need to pass to construct this distribution)
     */
    std::normal_distribution<float> gaus_distribution(0, 20.0f / 4.29193f);

    struct rel_pos {
        int x, y;
    } neighbour_position[] = {
        - 1, - 1,
          0, - 1,
          1, - 1,
        - 1,   0,
          1,   0,
        - 1,   1,
          0,   1,
          1,   1,
    };

#define in_depth_at(x, y) *(in_depth_px + width * y + x)
#define in_label_at(x, y) *(in_labels_px + width * y + x)
#define out_depth_at(x, y) *(out_depth_px + width * y + x)
#define out_label_at(x, y) *(out_labels_px + width * y + x)

    memcpy(noisy_labels->data_u8, labels->data_u8, labels->stride);
    memcpy(noisy_depth->data_float, depth->data_float, depth->stride);

    for (int y = 1; y < height - 2; y++) {
        for (int x = 1; x < width - 2; x++) {
#if 1
            if (in_label_at(x, y) != BACKGROUND_ID) {
                bool edge = false;
                uint8_t neighbour_label[8] = {
                    in_label_at(x - 1, y - 1),
                    in_label_at(x,     y - 1),
                    in_label_at(x + 1, y - 1),
                    in_label_at(x - 1, y),
                    in_label_at(x + 1, y),
                    in_label_at(x - 1, y + 1),
                    in_label_at(x,     y + 1),
                    in_label_at(x + 1, y + 1),
                };

                for (int i = 0; i < 8; i++) {
                    if (neighbour_label[i] != BACKGROUND_ID) {
                        edge = true;
                        break;
                    }
                }

                if (edge) {
                    int neighbour = uniform_distribution(rand_generator);
                    out_label_at(x, y) = neighbour_label[neighbour];

                    struct rel_pos *rel_pos = &neighbour_position[neighbour];
                    out_depth_at(x, y) = in_depth_at(x + rel_pos->x, y + rel_pos->y);
                } else {
                    out_label_at(x, y) = in_label_at(x, y);
                    out_depth_at(x, y) = in_depth_at(x, y);
                }
            } else {
                out_label_at(x, y) = in_label_at(x, y);
                out_depth_at(x, y) = in_depth_at(x, y);
            }
#endif
        }
    }

    memcpy(noisy_labels->data_u8 + (height - 1) * width,
           labels->data_u8 + (height - 1) * width,
           labels->stride);
    memcpy(noisy_depth->data_float + (height - 1) * width,
           depth->data_float + (height - 1) * width,
           depth->stride);

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            if (in_label_at(x, y) != BACKGROUND_ID) {
                float delta_mm = gaus_distribution(rand_generator);
                out_depth_at(x, y) += (delta_mm / 1000.0f);
            } else {
                out_depth_at(x, y) = 20;
            }
        }
    }
#undef in_depth_at
#undef in_label_at
#undef out_depth_at
#undef out_label_at
}


static void
save_frame_depth(const char *dir, const char *filename,
                 struct image *depth)
{
    char output_filename[1024];
    struct stat st;

    xsnprintf(output_filename, "%s/depth/%s/%s", top_out_dir, dir, filename);

    if (stat(output_filename, &st) != -1) {
        fprintf(stderr, "Skipping EXR file %s as output already exist\n", output_filename);
        return;
    }

    write_exr(output_filename, depth);
    debug("wrote %s\n", output_filename);
}

static bool
save_frame_labels(const char *dir, const char *filename,
                  struct image *labels)
{
    int width = labels->width;
    int height = labels->height;
    int row_stride = labels->stride;
    char output_filename[1024];
    png_bytep rows[height];

    xsnprintf(output_filename, "%s/labels/%s/%s", top_out_dir, dir, filename);

    for (int y = 0; y < height; y++)
        rows[y] = (png_byte *)(labels->data_u8 + row_stride * y);

    struct stat st;

    if (stat(output_filename, &st) == -1) {
        if (!write_png_file(output_filename,
                            width, height,
                            rows,
                            PNG_COLOR_TYPE_PALETTE,
                            8)) { /* bit depth */
            return false;
        }
        debug("wrote %s\n", output_filename);
    } else {
        fprintf(stderr, "SKIP: %s file already exists\n",
                output_filename);
        return false;
    }

    return true;
}

static struct image *
load_frame_depth(const char *dir, const char *filename)
{
    char input_filename[1024];

    xsnprintf(input_filename, "%s/depth/%s/%s", top_src_dir, dir, filename);

    /* Just for posterity and to vent frustration within comments, the
     * RgbaInputFile and Rgba struct that the openexr documentation recommends
     * for reading typical RGBA EXR images is only good for half float
     * components.
     *
     * We noticed this after seeing lots of 'inf' float values due to out of
     * range floats.
     */
    InputFile in_file(input_filename);

    Box2i dw = in_file.header().dataWindow();

    int width = dw.max.x - dw.min.x + 1;
    int height = dw.max.y - dw.min.y + 1;

    struct image *depth = xalloc_image(IMAGE_FORMAT_XFLOAT, width, height);

    /* We assume the green and blue channels are redundant and arbitrarily
     * just pick the red channel to read...
     *
     * We're also assuming the channels aren't interleaved (does EXA support
     * that?)
     */
    FrameBuffer framebuffer;
    framebuffer.insert("R",
                       Slice(FLOAT,
                             (char *)depth->data_float,
                             sizeof(float), // x stride,
                             sizeof(float) * width)); // y stride

    in_file.setFrameBuffer(framebuffer);

#if 0 // uncomment to debug / check the channels available
    const ChannelList &channels = in_file.header().channels();
    for (ChannelList::ConstIterator i = channels.begin(); i != channels.end(); ++i) {
        const Channel &channel = i.channel();
        const char *name = i.name();

        debug("EXR: Channel '%s': type %s\n", name, channel.type == FLOAT ? "== FLOAT" : "!= FLOAT");
    }
#endif

    in_file.readPixels(dw.min.y, dw.max.y);

    debug("read %s/%s (%dx%d) OK\n", dir, filename, width, height);

    return depth;
}


static void
ensure_directory(const char *path)
{
    struct stat st;
    int ret;

    char *dirname_copy = strdup(path);
    char *parent = dirname(dirname_copy);

    if (strcmp(parent, ".") != 0 &&
        strcmp(parent, "..") != 0 &&
        strcmp(parent, "/") != 0)
    {
        ensure_directory(parent);
    }

    free(parent);

    ret = stat(path, &st);
    if (ret == -1) {
        int ret = mkdir(path, 0777);
        if (ret < 0) {
            fprintf(stderr, "Failed to create destination directory %s: %m\n", path);
            exit(1);
        }
    }
}

static void
directory_recurse(const char *rel_path)
{
    char label_src_path[1024];
    //char depth_src_path[1024];
    char label_dst_path[1024];
    char depth_dst_path[1024];

    struct stat st;
    DIR *label_dir;
    struct dirent *label_entry;
    char *ext;

    struct work *work = NULL;

    xsnprintf(label_src_path, "%s/labels/%s", top_src_dir, rel_path);
    //xsnprintf(depth_src_path, "%s/depth/%s", top_src_dir, rel_path);
    xsnprintf(label_dst_path, "%s/labels/%s", top_out_dir, rel_path);
    xsnprintf(depth_dst_path, "%s/depth/%s", top_out_dir, rel_path);

    ensure_directory(label_dst_path);
    ensure_directory(depth_dst_path);

    label_dir = opendir(label_src_path);

    while ((label_entry = readdir(label_dir)) != NULL) {
        char next_rel_path[1024];
        char next_src_label_path[1024];

        if (strcmp(label_entry->d_name, ".") == 0 ||
            strcmp(label_entry->d_name, "..") == 0)
            continue;

        xsnprintf(next_rel_path, "%s/%s", rel_path, label_entry->d_name);
        xsnprintf(next_src_label_path, "%s/labels/%s", top_src_dir, next_rel_path);

        stat(next_src_label_path, &st);
        if (S_ISDIR(st.st_mode)) {
            debug("%*srecursing into %s\n", indent, "", next_rel_path);
            indent += 2;
            directory_recurse(next_rel_path);
            indent -= 2;
        } else if ((ext = strstr(label_entry->d_name, ".png")) && ext[4] == '\0') {

            if (!work) {
                struct work empty;

                work_queue.push(empty);
                work = &work_queue.back();

                work->dir = strdup(rel_path);
                work->files = std::vector<char *>();
            }

            work->files.push_back(strdup(label_entry->d_name));
        }
    }

    closedir(label_dir);
}

static void *
worker_thread_cb(void *data)
{
    struct worker_state *state = (struct worker_state *)data;
    struct image *noisy_labels, *noisy_depth;
    struct image *flipped_labels, *flipped_depth;

    debug("Running worker thread\n");

    for (;;) {
        struct work work;

        char label_dir_path[1024];

        struct image *prev_frame_labels = NULL;

        pthread_mutex_lock(&work_queue_lock);
        if (!work_queue.empty()) {
            work = work_queue.front();
            work_queue.pop();
        } else {
            pthread_mutex_unlock(&work_queue_lock);
            debug("Worker thread finished\n");
            return NULL;
        }
        pthread_mutex_unlock(&work_queue_lock);

        xsnprintf(label_dir_path, "%s/labels/%s", top_src_dir, work.dir);

        for (unsigned i = 0; i < work.files.size(); i++) {
            debug("Thread %d: processing %s/%s\n", state->idx, work.dir, work.files[i]);

            struct image *labels = load_frame_labels(work.dir, work.files[i]);

            int n_different_px = 0, n_body_px = 0;
            if (prev_frame_labels) {
                bool differ = frame_diff(labels,
                                         prev_frame_labels,
                                         &n_different_px,
                                         &n_body_px);

                if (n_body_px == 0) {
                    fprintf(stderr, "Skipping spurious frame with not body pixels!\n");
                    free_image(labels);
                    continue;
                }

                if (!differ) {
                    fprintf(stderr, "SKIPPING: %s/%s - too similar to previous frame (only %d out of %d body pixels differ)\n",
                            work.dir, work.files[i],
                            n_different_px,
                            n_body_px);
                    free_image(labels);
                    continue;
                }
            }

            if (prev_frame_labels)
                free_image(prev_frame_labels);
            prev_frame_labels = labels;

            char filename[128];

            xsnprintf(filename, "%.*s.exr",
                      (int)strlen(work.files[i]) - 4,
                      work.files[i]);

            struct image *depth = load_frame_depth(work.dir, filename);

            if (!noisy_labels) {
                int width = labels->width;
                int height = labels->height;

                noisy_labels = xalloc_image(IMAGE_FORMAT_X8, width, height);
                noisy_depth = xalloc_image(IMAGE_FORMAT_XFLOAT, width, height);

                flipped_labels = xalloc_image(IMAGE_FORMAT_X8, width, height);
                flipped_depth = xalloc_image(IMAGE_FORMAT_XFLOAT, width, height);
            }

            frame_add_noise(labels, depth, noisy_labels, noisy_depth);

            save_frame_labels(work.dir, work.files[i], noisy_labels);
            save_frame_depth(work.dir, filename, noisy_depth);

            flip_frame_labels(labels, flipped_labels);
            flip_frame_depth(depth, flipped_depth);
            frame_add_noise(flipped_labels, flipped_depth, noisy_labels, noisy_depth);

            xsnprintf(filename, "%.*s-flipped.png",
                      (int)strlen(work.files[i]) - 4,
                      work.files[i]);
            save_frame_labels(work.dir, filename, noisy_labels);

            xsnprintf(filename, "%.*s-flipped.exr",
                      (int)strlen(work.files[i]) - 4,
                      work.files[i]);
            save_frame_depth(work.dir, filename, noisy_depth);

            // Note: we don't free the labels here because they are preserved
            // for comparing with the next frame
            free(depth);
        }

        if (prev_frame_labels) {
            free_image(prev_frame_labels);
            prev_frame_labels = NULL;
        }
    }

    return NULL;
}

static bool
read_file(const char *filename, void *buf, int max)
{
    int fd;
    int n;

    memset(buf, 0, max);

    fd = open(filename, 0);
    if (fd < 0)
        return false;
    n = read(fd, buf, max - 1);
    close(fd);
    if (n < 0)
        return false;

    return true;
}

static void
cpu_count_once_cb(void)
{
    char buf[32];
    unsigned ignore = 0, max_cpu = 0;

    if (!read_file("/sys/devices/system/cpu/present", buf, sizeof(buf)))
        return;

    if (sscanf(buf, "%u-%u", &ignore, &max_cpu) != 2)
        return;

    n_cpus = max_cpu + 1;
}

static int
cpu_count(void)
{
    pthread_once(&cpu_count_once, cpu_count_once_cb);

    return n_cpus;
}

static void
usage(void)
{
    printf("Usage image-pre-processor [options] <top_src> <top_dest>\n"
           "\n"
           "    --half              Write full-float channel depth images\n"
           "                        (otherwise writes half-float)\n"
           "\n"
           "    -h,--help           Display this help\n\n"
           "\n");
    exit(1);
}


int
main(int argc, char **argv)
{
    int opt;

    /* N.B. The initial '+' means that getopt will stop looking for options
     * after the first non-option argument...
     */
    const char *short_options="+hf";
    const struct option long_options[] = {
        {"help",            no_argument,        0, 'h'},
        {"full",            no_argument,        0, 'f'},
        {0, 0, 0, 0}
    };

    while ((opt = getopt_long(argc, argv, short_options, long_options, NULL))
           != -1)
    {
        switch (opt) {
            case 'h':
                usage();
                return 0;
            case 'f':
                write_half_float = false;
                break;
        }
    }

    if (optind != argc - 2)
        usage();

    grey_to_id_map[0x07] = 0; // head left
    grey_to_id_map[0x0f] = 1; // head right
    grey_to_id_map[0x16] = 2; // head top left
    grey_to_id_map[0x1d] = 3; // head top right
    grey_to_id_map[0x24] = 4; // neck
    grey_to_id_map[0x2c] = 5; // clavicle left
    grey_to_id_map[0x33] = 6; // clavicle right
    grey_to_id_map[0x3a] = 7; // shoulder left
    grey_to_id_map[0x42] = 8; // upper-arm left
    grey_to_id_map[0x49] = 9; // shoulder right
    grey_to_id_map[0x50] = 10; // upper-arm right
    grey_to_id_map[0x57] = 11; // elbow left
    grey_to_id_map[0x5f] = 12; // forearm left
    grey_to_id_map[0x66] = 13; // elbow right
    grey_to_id_map[0x6d] = 14; // forearm right
    grey_to_id_map[0x75] = 15; // left wrist
    grey_to_id_map[0x7c] = 16; // left hand
    grey_to_id_map[0x83] = 17; // right wrist
    grey_to_id_map[0x8a] = 18; // right hand
    grey_to_id_map[0x92] = 19; // left hip
    grey_to_id_map[0x99] = 20; // left thigh
    grey_to_id_map[0xa0] = 21; // right hip
    grey_to_id_map[0xa8] = 22; // right thigh
    grey_to_id_map[0xaf] = 23; // left knee
    grey_to_id_map[0xb6] = 24; // left shin
    grey_to_id_map[0xbd] = 25; // right knee
    grey_to_id_map[0xc5] = 26; // right shin
    grey_to_id_map[0xcc] = 27; // left ankle
    grey_to_id_map[0xd3] = 28; // left toes
    grey_to_id_map[0xdb] = 29; // right ankle
    grey_to_id_map[0xe2] = 30; // right toes
    grey_to_id_map[0xe9] = 31; // left waist
    grey_to_id_map[0xf0] = 32; // right waist

static_assert(BACKGROUND_ID == 33, "");
    grey_to_id_map[0x40] = BACKGROUND_ID;

    // A few paranoid checks...
    static_assert(MAX_PACKED_INDEX == 33, "Only expecting 33 labels");
    static_assert(ARRAY_LEN(left_to_right_map) == (MAX_PACKED_INDEX + 1),
                  "Only expecting to flip 33 packed labels");

    for (unsigned i = 0; i < ARRAY_LEN(left_to_right_map); i++)
        left_to_right_map[i] = i;

#define flip(A, B) do {  \
        uint8_t tmp = left_to_right_map[A]; \
        left_to_right_map[A] = left_to_right_map[B]; \
        left_to_right_map[B] = tmp; \
    } while(0)

    flip(0, 1); //head
    flip(2, 3); // head top
    flip(5, 6); // clavicle
    flip(7, 9); // shoulder
    flip(8, 10); // upper-arm
    flip(11, 13); // elbow
    flip(12, 14); // forearm
    flip(15, 17); // wrist
    flip(16, 18); // hand
    flip(19, 21); // hip
    flip(20, 22); // thigh
    flip(23, 25); // knee
    flip(24, 26); // shin
    flip(27, 29); // ankle
    flip(28, 30); // toes
    flip(31, 32); // waist

#undef flip

    top_src_dir = argv[optind];
    top_out_dir = argv[optind + 1];

    printf("Queuing frames to process...\n");

    uint64_t start = get_time();
    directory_recurse("" /* initially empty relative path */);
    uint64_t end = get_time();

    uint64_t duration_ns = end - start;
    printf("%d directories queued to process, in %.3f%s\n",
           (int)work_queue.size(),
           get_duration_ns_print_scale(duration_ns),
           get_duration_ns_print_scale_suffix(duration_ns));

    int n_cpus = cpu_count();
    int n_threads = n_cpus * 32;

    //n_threads = 1;

    printf("Spawning %d worker threads\n", n_threads);

    workers.resize(n_threads, worker_state());

    start = get_time();

    for (int i = 0; i < n_threads; i++) {
        workers[i].idx = i;
        pthread_create(&workers[i].thread,
                       NULL, //sttributes
                       worker_thread_cb,
                       &workers[i]); //data
    }

    for (int i = 0; i < n_threads; i++) {
        void *ret;

        pthread_join(workers[i].thread, &ret);
    }

    end = get_time();
    duration_ns = end - start;

    printf("Finished processing all frames in %.3f%s\n",
           get_duration_ns_print_scale(duration_ns),
           get_duration_ns_print_scale_suffix(duration_ns));

    return 0;
}
