#pragma once

// The converters yield with vTaskDelay(); on the host the run script compiles
// that to a no-op, so this header only has to exist for any transitive include.
