#ifndef REGISTRY_PRIV_H
#define REGISTRY_PRIV_H

#include "HalVerDef.h"
#include <cstdint>

struct registry_priv {
  constexpr static uint8_t channel = 36; /* ad-hoc support requirement */
  constexpr static HAL_RF_TYPE_E rf_config = RF_TYPE_MAX;
  constexpr static bool wifi_spec = false; /* !turbo_mode */
  constexpr static uint8_t special_rf_path =
      0; /* 0: 2T2R ,1: only turn on path A 1T1R */
  constexpr static int8_t TxBBSwing_2G = -1;
  constexpr static int8_t TxBBSwing_5G = -1;
  constexpr static uint8_t AmplifierType_2G = 0;
  constexpr static uint8_t AmplifierType_5G = 0;
  constexpr static uint8_t RFE_Type = 64;
};

#endif /* REGISTRY_PRIV_H */
