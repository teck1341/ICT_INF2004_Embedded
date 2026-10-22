/** @file app_cfg.h
 * @brief Convert explicit project hypotheses into subsystem configurations.
 */
#ifndef APP_CFG_H
#define APP_CFG_H

#include "motion.h"
#include "vehicle.h"
#include "line_ctrl.h"
#include "terrain.h"
#include "scanner.h"

/** Populate each configuration; confirmation remains an independent gate. */
void app_cfg_motion (motion_config_t * p_config);
void app_cfg_vehicle (vehicle_config_t * p_config, bool b_codebook_ready);
void app_cfg_line (line_ctrl_config_t * p_config);
void app_cfg_terrain (terrain_config_t * p_config);
void app_cfg_scanner (scanner_config_t * p_config);

#endif /* APP_CFG_H */

/* End of file. */

