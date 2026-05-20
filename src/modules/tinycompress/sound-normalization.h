#ifndef _SOUND_NORMALIZATION_H_
#define _SOUND_NORMALIZATION_H_

#include <pulsecore/macro.h>

#define MIXER_CARD 0

typedef struct pa_normalizer_conf{
    bool    offload_normalizer_enable;
    bool    offload_normalizer_devicespeaker;
    double  offload_normalizer_makeupgain;
    bool    offload_normalizer_prefilter;
    int     offload_normalizer_limiterthreshold;
    double  offload_normalizer_limiterslope;
    int     offload_normalizer_compressorthreshold;
    double  offload_normalizer_compressorslope;
    bool    offload_normalizer_onoff;
    char    *config_file;
}pa_normalizer_conf;

struct lge_normalizer_lastvalue {
    int enable;
    int devicespeaker;
    int makeupgain;
    int prefilter;
    int limiterthreshold;
    int limiterslope;
    int compressorthreshold;
    int compressorslope;
    int onoff;
    int start_offload;
};

int adev_set_parameters(int  );
int LGESoundNormalizer_init(void );
int LGESoundNormalizer_set_parameters(void );
int LGESoundNormalizer_close(void );
int LGESoundNormalizer_reset(int val);

enum SNSTATUS{
        E_SUCCESS=0,
        E_NODATA,
        E_INVALID
};

#endif
