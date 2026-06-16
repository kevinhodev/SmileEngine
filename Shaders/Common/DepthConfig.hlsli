#ifndef SMILE_DEPTH_CONFIG
#define SMILE_DEPTH_CONFIG

#define SMILE_REVERSE_Z 1

#if SMILE_REVERSE_Z
    #define SMILE_NDC_FAR 0.0f   
#else
    #define SMILE_NDC_FAR 1.0f
#endif

bool SmileIsSky(float d) {
#if SMILE_REVERSE_Z
    return d <= 0.0f;
#else
    return d >= 1.0f;
#endif
}

#endif 
