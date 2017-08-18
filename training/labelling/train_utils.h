
#ifndef __TRAIN_UTILS__
#define __TRAIN_UTILS__

#include <stdint.h>
#include <stdbool.h>

void gather_train_data(const char* label_dir_path,
                       const char* depth_dir_path,
                       uint32_t    limit,
                       bool        shuffle,
                       uint32_t*   out_n_images,
                       int32_t*    out_width,
                       int32_t*    out_height,
                       float**     out_depth_images,
                       uint8_t**   out_label_images);

#endif /* __TRAIN_UTILS__ */
