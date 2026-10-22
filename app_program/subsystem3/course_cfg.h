/** @file course_cfg.h
 * @brief Course-specific contract; never infer encoding from example letters.
 */
#ifndef COURSE_CFG_H
#define COURSE_CFG_H

#include "barcode.h"

/** Reviewed codebook as a constant table in flash (no RAM copy); never
 * NULL. barcode_derive() converts it once. The supplied table is empty and
 * inhibited.
 */
const barcode_config_t * course_cfg_codebook (void);

#endif /* COURSE_CFG_H */

/* End of file. */

