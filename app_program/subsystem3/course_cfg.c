/** @file course_cfg.c
 * @brief Explicit integration point for instructor-confirmed barcode widths.
 * This is missing input, not a pretend decoder implementation: barcode.c
 * implements spatial decoding and is tested with labelled synthetic fixtures.
 */
#include "course_cfg.h"
#include "robot_config.h"

/* Enter measured symbols, framing, tolerances and encoder resolution here
 * before confirming. Do not copy test patterns onto the course. Widths stay
 * in metres (the narrow/wide encoding waits for the confirmed symbology);
 * barcode_derive() converts them to half-counts, and the widest run must be
 * at most 255 half-counts (72.3 mm with the current hypotheses).
 * reverse_tolerance_m must exceed one effective encoder count of centre
 * distance (2*pi*R/counts, about 0.567 mm with the current hypotheses): the
 * 0.5 mm default is below it (floor(0.5 / 0.2836) = 1 half-count), and the
 * start-up codebook gate in robot_app.c does not check this yet (T-57).
 */
static const barcode_config_t g_course_cfg_codebook =
{
    BARCODE_DEFAULT_LIMITS,
    .b_calibration_confirmed = (0 != ROBOT_CALIBRATION_CONFIRMED),
    .b_codebook_confirmed    = (0 != ROBOT_BARCODE_CONFIRMED)
};

/** @brief Empty codebook keeps autonomous START inhibited until configured. */
const barcode_config_t * course_cfg_codebook (void)
{
    return &g_course_cfg_codebook;
}

/* End of file. */

