#pragma once

#include "ofxsImageEffect.h"

// Forward declarations for HDR support
struct NDIlib_video_frame_v2_t;
struct NDIlib_metadata_frame_t;

class NDIOutputPluginFactory : public OFX::PluginFactoryHelper<NDIOutputPluginFactory>
{
public:
    NDIOutputPluginFactory();
    virtual void load() {}
    virtual void unload() {}
    virtual void describe(OFX::ImageEffectDescriptor& p_Desc);
    virtual void describeInContext(OFX::ImageEffectDescriptor& p_Desc, OFX::ContextEnum p_Context);
    virtual OFX::ImageEffect* createInstance(OfxImageEffectHandle p_Handle, OFX::ContextEnum p_Context);
}; 