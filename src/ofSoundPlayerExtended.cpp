
#include "ofSoundPlayerExtended.h"

#ifdef ENABLE_SOUND_PLAYER_EXTENDED
//#ifdef OF_SOUND_PLAYER_OPENAL

#include "ofUtils.h"
#include "ofMath.h"
#include "ofFileUtils.h"
#include "ofAppRunner.h"
#include <set>
#ifdef TARGET_WIN32
#include <math.h>
#endif

#if defined (TARGET_WIN32) || defined (TARGET_OSX)
#include "ofxAudioDecoder.h"
#endif

ALCdevice * ofSoundPlayerExtended::alDevice = 0;
ALCcontext * ofSoundPlayerExtended::alContext = 0;
//vector<float> ofSoundPlayerExtended::window;
//float ofSoundPlayerExtended::windowSum=0;




static set<ofSoundPlayerExtended*> players;

void ofOpenALSoundUpdate_TimelineAdditions(){
    alcProcessContext(ofSoundPlayerExtended::alContext);
}

static string getALErrorString(ALenum error) {
    switch(error) {
        case AL_NO_ERROR:
            return "AL_NO_ERROR";
        case AL_INVALID_NAME:
            return "AL_INVALID_NAME";
        case AL_INVALID_ENUM:
            return "AL_INVALID_ENUM";
        case AL_INVALID_VALUE:
            return "AL_INVALID_VALUE";
        case AL_INVALID_OPERATION:
            return "AL_INVALID_OPERATION";
        case AL_OUT_OF_MEMORY:
            return "AL_OUT_OF_MEMORY";
    };
    return "UNKWOWN_ERROR";
}


#define BUFFER_STREAM_SIZE 4096

// now, the individual sound player:
//------------------------------------------------------------
ofSoundPlayerExtended::ofSoundPlayerExtended(){
    bLoop 			= false;
    bLoadedOk 		= false;
    pan 			= 0.5f;
    volume 			= 1.0f;
    internalFreq 	= 44100;
    speed 			= 1;
    bPaused 		= false;
    isStreaming		= false;
    channels		= 0;
    duration		= 0;
    streamf			= 0;
    curMaxAverage   = 0;
    timeSet         = false;
#ifdef OF_USING_MPG123
    mp3streamf		= 0;
#endif
    players.insert(this);
}

// ----------------------------------------------------------------------------
ofSoundPlayerExtended::~ofSoundPlayerExtended(){
    unload();
    players.erase(this);
}

//---------------------------------------
// this should only be called once
void ofSoundPlayerExtended::initialize(){
    if(!alDevice){
        alDevice = alcOpenDevice(NULL);
        alContext = alcCreateContext(alDevice,NULL);
        alcMakeContextCurrent (alContext);
        alListener3f(AL_POSITION, 0,0,0);
#ifdef OF_USING_MPG123
        mpg123_init();
#endif
        
    }
}

//---------------------------------------
void ofSoundPlayerExtended::close(){
    if(alDevice){
        alcCloseDevice(alDevice);
        alDevice = NULL;
        alcDestroyContext(alContext);
        alContext = 0;
#ifdef OF_USING_MPG123
        mpg123_exit();
#endif
    }
}

// ----------------------------------------------------------------------------
bool ofSoundPlayerExtended::sfReadFile(const std::filesystem::path& path, vector<short> & buffer, vector<float> & fftAuxBuffer){
    SF_INFO sfInfo;
    SNDFILE* f = sf_open(path.c_str(),SFM_READ,&sfInfo);
    if(!f){
        ofLogError("ofSoundPlayerExtended") << "sfReadFile(): couldn't read \"" << path << "\"";
        return false;
    }
    
    buffer.resize(sfInfo.frames*sfInfo.channels);
    fftAuxBuffer.resize(sfInfo.frames*sfInfo.channels);
    
    int subformat = sfInfo.format & SF_FORMAT_SUBMASK ;
    if (subformat == SF_FORMAT_FLOAT || subformat == SF_FORMAT_DOUBLE){
        double	scale ;
        sf_command (f, SFC_CALC_SIGNAL_MAX, &scale, sizeof (scale)) ;
        if (scale < 1e-10)
            scale = 1.0 ;
        else
            scale = 32700.0 / scale ;
        
        sf_count_t samples_read = sf_read_float (f, &fftAuxBuffer[0], fftAuxBuffer.size());
        if(samples_read<(int)fftAuxBuffer.size())
            ofLogWarning("ofSoundPlayerExtended") << "sfReadFile(): read " << samples_read << " float samples, expected "
            << fftAuxBuffer.size() << " for \"" << path << "\"";
        for (int i = 0 ; i < int(fftAuxBuffer.size()) ; i++){
            fftAuxBuffer[i] *= scale ;
            buffer[i] = 32565.0 * fftAuxBuffer[i];
        }
    }else{
        sf_count_t frames_read = sf_readf_short(f,&buffer[0],sfInfo.frames);
        if(frames_read<sfInfo.frames){
            ofLogError("ofSoundPlayerExtended") << "sfReadFile(): read " << frames_read << " frames from buffer, expected "
            << sfInfo.frames << " for \"" << path << "\"";
            return false;
        }
        sf_seek(f,0,SEEK_SET);
        frames_read = sf_readf_float(f,&fftAuxBuffer[0],sfInfo.frames);
        if(frames_read<sfInfo.frames){
            ofLogError("ofSoundPlayerExtended") << "sfReadFile(): read " << frames_read << " frames from fft buffer, expected "
            << sfInfo.frames << " for \"" << path << "\"";
            return false;
        }
    }
    sf_close(f);
    
    channels = sfInfo.channels;
    duration = float(sfInfo.frames) / float(sfInfo.samplerate);
    samplerate = sfInfo.samplerate;
    return true;
}

#ifdef OF_USING_MPG123
//------------------------------------------------------------
bool ofSoundPlayerExtended::mpg123ReadFile(const std::filesystem::path& path,std::vector<short> & buffer,std::vector<float> & fftAuxBuffer){ 
    int err = MPG123_OK;
    mpg123_handle * f = mpg123_new(NULL,&err);
    if(mpg123_open(f,path.c_str())!=MPG123_OK){
        return false;
    }
    
    int encoding;
    long int rate;
    mpg123_getformat(f,&rate,&channels,&encoding);
    if(encoding!=MPG123_ENC_SIGNED_16){
        ofLog(OF_LOG_ERROR,"ofSoundPlayerExtended: unsupported encoding");
        return false;
    }
    samplerate = rate;
    
    size_t done=0;
    size_t buffer_size = mpg123_outblock( f );
    buffer.resize(buffer_size/2);
    while(mpg123_read(f,(unsigned char*)&buffer[buffer.size()-buffer_size/2],buffer_size,&done)!=MPG123_DONE){
        buffer.resize(buffer.size()+buffer_size/2);
    };
    buffer.resize(buffer.size()-(buffer_size/2-done/2));
    mpg123_close(f);
    mpg123_delete(f);
    
    fftAuxBuffer.resize(buffer.size());
    for(int i=0;i<(int)buffer.size();i++){
        fftAuxBuffer[i] = float(buffer[i])/32565.f;
    }
    duration = float(buffer.size()/channels) / float(samplerate);
    return true;
}
#endif

//------------------------------------------------------------
bool ofSoundPlayerExtended::decoderReadFile(const std::filesystem::path& path,vector<short> & buffer,vector<float> & fftAuxBuffer){
    
    
    #if defined (TARGET_WIN32) || defined (TARGET_OSX)
    std::string fileName = ofToDataPath(path);
    
    ofxAudioDecoder audioDecoder;
    audioDecoder.load(fileName);
    
    if (audioDecoder.getNumSamples() == 0) {
        ofLogError("ofSoundPlayerExtended") << "couldn't read \"" << path << "\"";
        return false;
    }
    
    buffer.resize(audioDecoder.getNumFrames() * audioDecoder.getNumChannels());
    fftAuxBuffer.resize(audioDecoder.getNumFrames() * audioDecoder.getNumChannels());
    
    memcpy(fftAuxBuffer.data(), audioDecoder.getRawSamples().data(), audioDecoder.getNumSamples() * sizeof(float));
    
    for (int i = 0; i < fftAuxBuffer.size(); ++i) {
        buffer[i] = 32565.0 * fftAuxBuffer[i];
    }
    
    channels = audioDecoder.getNumChannels();
    duration = float(audioDecoder.getNumFrames()) / float(audioDecoder.getSampleRate());
    samplerate = audioDecoder.getSampleRate();
    return true;
    
    #endif

    return false;
}

//------------------------------------------------------------
bool ofSoundPlayerExtended::sfStream(const std::filesystem::path& path, vector<short> & buffer,vector<float> & fftAuxBuffer){
    if(!streamf){
        SF_INFO sfInfo;
        streamf = sf_open(path.c_str(),SFM_READ,&sfInfo);
        if(!streamf){
            ofLogError("ofSoundPlayerExtended") << "sfStream(): couldn't read \"" << path << "\"";
            return false;
        }
        
        stream_subformat = sfInfo.format & SF_FORMAT_SUBMASK ;
        if (stream_subformat == SF_FORMAT_FLOAT || stream_subformat == SF_FORMAT_DOUBLE){
            sf_command (streamf, SFC_CALC_SIGNAL_MAX, &stream_scale, sizeof (stream_scale)) ;
            if (stream_scale < 1e-10)
                stream_scale = 1.0 ;
            else
                stream_scale = 32700.0 / stream_scale ;
        }
        channels = sfInfo.channels;
        duration = float(sfInfo.frames) / float(sfInfo.samplerate);
        samplerate = sfInfo.samplerate;
        stream_samples_read = 0;
    }
    
    int curr_buffer_size = BUFFER_STREAM_SIZE*channels;
    if(speed>1) curr_buffer_size *= (int)floor(speed+.5);
    buffer.resize(curr_buffer_size);
    fftAuxBuffer.resize(buffer.size());
    if (stream_subformat == SF_FORMAT_FLOAT || stream_subformat == SF_FORMAT_DOUBLE){
        sf_count_t samples_read = sf_read_float (streamf, &fftAuxBuffer[0], fftAuxBuffer.size());
        stream_samples_read += samples_read;
        if(samples_read<(int)fftAuxBuffer.size()){
            fftAuxBuffer.resize(samples_read);
            buffer.resize(samples_read);
            setPosition(0);
            if(!bLoop) stopThread();
            stream_samples_read = 0;
            stream_end = true;
        }
        for (int i = 0 ; i < int(fftAuxBuffer.size()) ; i++){
            fftAuxBuffer[i] *= stream_scale ;
            buffer[i] = 32565.0 * fftAuxBuffer[i];
        }
    }else{
        sf_count_t frames_read = sf_readf_short(streamf,&buffer[0],curr_buffer_size/channels);
        stream_samples_read += frames_read*channels;
        if(frames_read<curr_buffer_size/channels){
            fftAuxBuffer.resize(frames_read*channels);
            buffer.resize(frames_read*channels);
            setPosition(0);
            if(!bLoop) stopThread();
            stream_samples_read = 0;
            stream_end = true;
        }
        for(int i=0;i<(int)buffer.size();i++){
            fftAuxBuffer[i]=float(buffer[i])/32565.0f;
        }
    }
    
    return true;
}

#ifdef OF_USING_MPG123
//------------------------------------------------------------
bool ofSoundPlayerExtended::mpg123Stream(const std::filesystem::path& path,vector<short> & buffer,vector<float> & fftAuxBuffer){
    if(!mp3streamf){
        int err = MPG123_OK;
        mp3streamf = mpg123_new(NULL,&err);
        if(mpg123_open(mp3streamf,path.c_str())!=MPG123_OK){
            mpg123_close(mp3streamf);
            mpg123_delete(mp3streamf);
             ofLogError("ofSoundPlayerExtended") << "mpg123Stream(): couldn't read \"" << path << "\"";
            return false;
        }
        
        long int rate;
        mpg123_getformat(mp3streamf,&rate,&channels,&stream_encoding);
        if(stream_encoding!=MPG123_ENC_SIGNED_16){
            return false;
        }
        samplerate = rate;
        mp3_buffer_size = mpg123_outblock( mp3streamf );
        
        
        mpg123_seek(mp3streamf,0,SEEK_END);
        off_t samples = mpg123_tell(mp3streamf);
        duration = float(samples/channels) / float(samplerate);
        mpg123_seek(mp3streamf,0,SEEK_SET);
    }
    
    int curr_buffer_size = mp3_buffer_size;
    if(speed>1) curr_buffer_size *= (int)round(speed);
    buffer.resize(curr_buffer_size);
    fftAuxBuffer.resize(buffer.size());
    size_t done=0;
    if(mpg123_read(mp3streamf,(unsigned char*)&buffer[0],curr_buffer_size*2,&done)==MPG123_DONE){
        setPosition(0);
        buffer.resize(done/2);
        fftAuxBuffer.resize(done/2);
        if(!bLoop) stopThread();
        stream_end = true;
    }
    
    
    for(int i=0;i<(int)buffer.size();i++){
        fftAuxBuffer[i] = float(buffer[i])/32565.f;
    }
    
    return true;
}
#endif

//------------------------------------------------------------
bool ofSoundPlayerExtended::stream(const std::filesystem::path& fileName, vector<short> & buffer){
#ifdef OF_USING_MPG123
    if(ofFilePath::getFileExt(fileName)=="mp3" || ofFilePath::getFileExt(fileName)=="MP3" || mp3streamf){
        if(!mpg123Stream(fileName,buffer,fftAuxBuffer)) return false;
    }else
#endif
        if(!sfStream(fileName,buffer,fftAuxBuffer)) return false;
    
   
    int numFrames = buffer.size()/channels;
    return true;
    
}

bool ofSoundPlayerExtended::readFile(const std::filesystem::path& fileName, vector<short> & buffer){
    if(ofFilePath::getFileExt(fileName)!="mp3" && ofFilePath::getFileExt(fileName)!="MP3"){
        if(!sfReadFile(fileName,buffer,fftAuxBuffer)) return false;
    }else{
#ifdef OF_USING_MPG123
        if(!mpg123ReadFile(fileName,buffer,fftAuxBuffer)) return false;
#else
        if(!decoderReadFile(fileName,buffer,fftAuxBuffer)) return false;
#endif
    }
    
    int numFrames = buffer.size()/channels;
    return true;
}

//------------------------------------------------------------
bool ofSoundPlayerExtended::load(const std::filesystem::path& _fileName, bool is_stream){
    
    std::filesystem::path fileName = ofToDataPath(_fileName);
    
    string ext = ofToLower(ofFilePath::getFileExt(fileName));
    if(ext != "wav" && ext != "aif" && ext != "aiff" && ext != "mp3"){
        ofLogError("Sound player can only load .wav .aiff or .mp3 files");
        return false;
    }
    
    fileName = ofToDataPath(fileName);
    
    bLoadedOk = false;
    bMultiPlay = false;
    isStreaming = is_stream;
    int err = AL_NO_ERROR;
    
    // [1] init sound systems, if necessary
    initialize();
    
    // [2] try to unload any previously loaded sounds
    // & prevent user-created memory leaks
    // if they call "loadSound" repeatedly, for example
    
    unload();
    
    ALenum format=AL_FORMAT_MONO16;
    
    if(!isStreaming || ext == "mp3"){ // mp3s don't stream cause they gotta be decoded
        readFile(fileName, buffer);
    }else{
        stream(fileName, buffer);
    }
    
    if(channels == 0){
        ofLogError("ofSoundPlayerExtended -- File not found");
        return false;
    }
    
    int numFrames = buffer.size()/channels;
    if(isStreaming){
        buffers.resize(channels*2);
    }else{
        buffers.resize(channels);
    }
    alGenBuffers(buffers.size(), &buffers[0]);
    if(channels==1){
        sources.resize(1);
        alGenSources(1, &sources[0]);
        err = alGetError();
        if (err != AL_NO_ERROR){
            ofLogError("ofSoundPlayerExtended") << "loadSound(): couldn't generate source for \"" << fileName << "\": "
            << (int) err << " " << getALErrorString(err);
            return false;
        }
        
        for(int i=0; i<(int)buffers.size(); i++){
            alGetError(); // Clear error.
            alBufferData(buffers[i],format,&buffer[0],buffer.size()*2,samplerate);
            err = alGetError();
            if (err != AL_NO_ERROR){
                ofLogError("ofSoundPlayerExtended:") << "loadSound(): couldn't create buffer for \"" << fileName << "\": "
                << (int) err << " " << getALErrorString(err);
                return false;
            }
            if(isStreaming){
                stream(fileName,buffer);
            }
        }
        if(isStreaming){
            alSourceQueueBuffers(sources[0],buffers.size(),&buffers[0]);
        }else{
            alSourcei (sources[0], AL_BUFFER,   buffers[0]);
        }
        
        alSourcef (sources[0], AL_PITCH,    1.0f);
        alSourcef (sources[0], AL_GAIN,     1.0f);
        alSourcef (sources[0], AL_ROLLOFF_FACTOR,  0.0);
        alSourcei (sources[0], AL_SOURCE_RELATIVE, AL_TRUE);
    }else{
        vector<vector<short> > multibuffer;
        multibuffer.resize(channels);
        sources.resize(channels);
        alGenSources(channels, &sources[0]);
        if(isStreaming){
            for(int s=0; s<2;s++){
                for(int i=0;i<channels;i++){
                    multibuffer[i].resize(buffer.size()/channels);
                    for(int j=0;j<numFrames;j++){
                        multibuffer[i][j] = buffer[j*channels+i];
                    }
                    alGetError(); // Clear error.
                    alBufferData(buffers[s*2+i],format,&multibuffer[i][0],buffer.size()/channels*2,samplerate);
                    err = alGetError();
                    if (err != AL_NO_ERROR){
                        ofLogError("ofSoundPlayerExtended") << "loadSound(): couldn't create stereo buffers for \"" << fileName << "\": " << (int) err << " " << getALErrorString(err);
                        return false;
                    }
                    alSourceQueueBuffers(sources[i],1,&buffers[s*2+i]);
                    stream(fileName,buffer);
                }
            }
        }else{
            for(int i=0;i<channels;i++){
                multibuffer[i].resize(buffer.size()/channels);
                for(int j=0;j<numFrames;j++){
                    multibuffer[i][j] = buffer[j*channels+i];
                }
                alGetError(); // Clear error.
                alBufferData(buffers[i],format,&multibuffer[i][0],buffer.size()/channels*2,samplerate);
                err = alGetError();
                if (err != AL_NO_ERROR){
                    ofLogError("ofSoundPlayerExtended") << "loadSound(): couldn't create stereo buffers for \"" << fileName << "\": "
                    << (int) err << " " << getALErrorString(err);
                    return false;
                }
                alSourcei (sources[i], AL_BUFFER,   buffers[i]   );
            }
        }
        
        for(int i=0;i<channels;i++){
            err = alGetError();
            if (err != AL_NO_ERROR){
                ofLogError("ofOpenALSoundPlayer_TimelineAdditions") << "loadSound(): couldn't create stereo sources for \"" << fileName << "\": "
                << (int) err << " " << getALErrorString(err);
                return false;
            }
            
            // only stereo panning
            if(i==0){
                float pos[3] = {-1,0,0};
                alSourcefv(sources[i],AL_POSITION,pos);
            }else{
                float pos[3] = {1,0,0};
                alSourcefv(sources[i],AL_POSITION,pos);
            }
            alSourcef (sources[i], AL_ROLLOFF_FACTOR,  0.0);
            alSourcei (sources[i], AL_SOURCE_RELATIVE, AL_TRUE);
        }
    }
    //soundBuffer stuff---------------------
    currentSoundBuffer.setNumChannels(channels);
    currentSoundBuffer.setSampleRate(samplerate);
    currentSoundBuffer.clear();
    channelSoundBuffer.setNumChannels(1);
    channelSoundBuffer.setSampleRate(samplerate);
    channelSoundBuffer.clear();
    //--------------------------------------
    ofLogVerbose("ofSoundPlayerExtended: successfully loaded: ") << fileName;
    bLoadedOk = true;
    return true;
}

//------------------------------------------------------------
void ofSoundPlayerExtended::threadedFunction(){
    vector<vector<short> > multibuffer;
    multibuffer.resize(channels);
    while(isThreadRunning()){
        cout << "threaded function" << endl;
        for(int i=0; i<int(sources.size())/channels; i++){
            int processed;
            alGetSourcei(sources[i*channels], AL_BUFFERS_PROCESSED, &processed);
            
            while(processed--)
            {
                stream("",buffer);
                int numFrames = buffer.size()/channels;
                if(channels>1){
                    for(int j=0;j<channels;j++){
                        multibuffer[j].resize(buffer.size()/channels);
                        for(int k=0;k<numFrames;k++){
                            multibuffer[j][k] = buffer[k*channels+j];
                        }
                        ALuint albuffer;
                        alSourceUnqueueBuffers(sources[i*channels+j], 1, &albuffer);
                        alBufferData(albuffer,AL_FORMAT_MONO16,&multibuffer[j][0],buffer.size()*2/channels,samplerate);
                        alSourceQueueBuffers(sources[i*channels+j], 1, &albuffer);
                    }
                }else{
                    ALuint albuffer;
                    alSourceUnqueueBuffers(sources[i], 1, &albuffer);
                    alBufferData(albuffer,AL_FORMAT_MONO16,&buffer[0],buffer.size()*2/channels,samplerate);
                    alSourceQueueBuffers(sources[i], 1, &albuffer);
                }
                if(stream_end){
                    break;
                }
            }
            ALint state;
            alGetSourcei(sources[i*channels],AL_SOURCE_STATE,&state);
            bool stream_running=false;
#ifdef OF_USING_MPG123
            stream_running = streamf || mp3streamf;
#else
            stream_running = streamf;
#endif
            if(state != AL_PLAYING && stream_running && !stream_end){
                alSourcePlayv(channels,&sources[i*channels]);
            }
            
            stream_end = false;
        }
        timeSet = false;
        
        ofSleepMillis(1);
    }
}

//------------------------------------------------------------
void ofSoundPlayerExtended::update(ofEventArgs & args){
    if(bMultiPlay){
        for(int i=1; i<int(sources.size())/channels; ){
            ALint state;
            alGetSourcei(sources[i*channels],AL_SOURCE_STATE,&state);
            if(state != AL_PLAYING){
                alDeleteSources(channels,&sources[i*channels]);
                for(int j=0;j<channels;j++){
                    sources.erase(sources.begin()+i*channels);
                }
            }else{
                i++;
            }
        }
    }
    timeSet = false;
}

//------------------------------------------------------------
void ofSoundPlayerExtended::unload(){
    
    //	ofRemoveListener(ofEvents.update,this,&ofSoundPlayerExtended::update);
    if(isLoaded()){
        ofRemoveListener(ofEvents().update,this,&ofSoundPlayerExtended::update);
        
        alDeleteBuffers(buffers.size(),&buffers[0]);
        alDeleteSources(sources.size(),&sources[0]);
        
        bLoadedOk = false;
    }
    streamf = 0;
}

//------------------------------------------------------------
bool ofSoundPlayerExtended::isPlaying() const{
    if(sources.empty()) return false;
    if(isStreaming) return isThreadRunning();
    ALint state;
    bool playing=false;
    for(int i=0;i<(int)sources.size();i++){
        alGetSourcei(sources[i],AL_SOURCE_STATE,&state);
        playing |= (state == AL_PLAYING);
    }
    return playing;
}

//------------------------------------------------------------
bool ofSoundPlayerExtended::isPaused() const{
    if(sources.empty()) return false;
    ALint state;
    bool paused=true;
    for(int i=0;i<(int)sources.size();i++){
        alGetSourcei(sources[i],AL_SOURCE_STATE,&state);
        paused &= (state == AL_PAUSED);
    }
    return paused;
}

//------------------------------------------------------------
float ofSoundPlayerExtended::getSpeed() const{
    return speed;
}

//------------------------------------------------------------
float ofSoundPlayerExtended::getPan() const{
    return pan;
}

float ofSoundPlayerExtended::getDuration() const{
    return duration;
}

int ofSoundPlayerExtended::getNumChannels() const{
    return channels;
}

int ofSoundPlayerExtended::getSampleRate() const{
    return samplerate;
}
//------------------------------------------------------------
vector<short> & ofSoundPlayerExtended::getBuffer(){
    return buffer;
}

//------------------------------------------------------------
void ofSoundPlayerExtended::setVolume(float vol){
    volume = vol;
    if(sources.empty()) return;
    if(channels==1){
        alSourcef (sources[sources.size()-1], AL_GAIN, vol);
    }else{
        setPan(pan);
    }
}

//------------------------------------------------------------
float ofSoundPlayerExtended::getVolume() const{
    return volume;
}

//------------------------------------------------------------
bool ofSoundPlayerExtended::isLoaded() const{
    return bLoadedOk;
}

//------------------------------------------------------------
void ofSoundPlayerExtended::setPosition(float pct){
    if(sources.empty()) return;
#ifdef OF_USING_MPG123
    if(mp3streamf){
        mpg123_seek(mp3streamf,duration*pct*samplerate*channels,SEEK_SET);
    }else
#endif
        if(streamf){
            sf_seek(streamf,duration*pct*samplerate*channels,SEEK_SET);
            stream_samples_read = 0;
        }else{
            for(int i=0;i<(int)channels;i++){
                alSourcef(sources[sources.size()-channels+i],AL_SEC_OFFSET,pct*duration);
            }
        }
    timeSet = true;
    justSetTime = pct;
}

//------------------------------------------------------------
void ofSoundPlayerExtended::setPositionMS(int ms){
    if(duration == 0) return;
    
    setPosition( ms / 1000. / duration);
}

//------------------------------------------------------------
float ofSoundPlayerExtended::getPosition() const{
    if(duration==0) return 0;
    if(sources.empty()) return 0;
    float pos;
#ifdef OF_USING_MPG123
    if(mp3streamf){
        pos = float(mpg123_tell(mp3streamf)) / float(channels) / float(samplerate);
    }else
#endif
        if(streamf){
            pos = float(stream_samples_read) / float(channels) / float(samplerate);
            return pos/duration;
        }else{
            if(timeSet) return justSetTime;
            alGetSourcef(sources[sources.size()-1],AL_SAMPLE_OFFSET,&pos);
            return channels*(pos/buffer.size());
            
            //alGetSourcef(sources[sources.size()-1],AL_SEC_OFFSET,&pos);
            //return pos / duration;
        }
    return 0.0;
}

//------------------------------------------------------------
int ofSoundPlayerExtended::getPositionMS() const{
    if(duration==0) return 0;
    if(sources.empty()) return 0;
    int pos;
#ifdef OF_USING_MPG123
    if(mp3streamf){
        pos = 1000 * float(mpg123_tell(mp3streamf)) / float(channels) / float(samplerate);
    }else
#endif
        if(streamf){
            pos = float(stream_samples_read) / float(channels) / float(samplerate);
            return pos * 1000;
        }else{
            float sampleOffset;
            alGetSourcef(sources[sources.size()-1],AL_SAMPLE_OFFSET,&sampleOffset);
            return 1000 * duration * channels * (sampleOffset/buffer.size());
        }
    return pos;
}

//------------------------------------------------------------
void ofSoundPlayerExtended::setPan(float p){
    if(sources.empty()) return;
    if(channels==1){
        p=p*2-1;
        float pos[3] = {p,0,0};
        alSourcefv(sources[sources.size()-1],AL_POSITION,pos);
    }else{
        for(int i=0;i<(int)channels;i++){
            if(i==0){
                alSourcef(sources[sources.size()-channels+i],AL_GAIN,(1-p)*volume);
            }else{
                alSourcef(sources[sources.size()-channels+i],AL_GAIN,p*volume);
            }
        }
    }
    pan = p;
}


//------------------------------------------------------------
void ofSoundPlayerExtended::setPaused(bool bP){
    if(sources.empty()) return;
    if(bP){
        alSourcePausev(sources.size(),&sources[0]);
    }else{
        alSourcePlayv(sources.size(),&sources[0]);
    }
    
    bPaused = bP;
}


//------------------------------------------------------------
void ofSoundPlayerExtended::setSpeed(float spd){
    for(int i=0;i<channels;i++){
        alSourcef(sources[sources.size()-channels+i],AL_PITCH,spd);
    }
    speed = spd;
}


//------------------------------------------------------------
void ofSoundPlayerExtended::setLoop(bool bLp){
    if(bMultiPlay) return; // no looping on multiplay
    bLoop = bLp;
    if(isStreaming) return;
    for(int i=0;i<(int)sources.size();i++){
        alSourcei(sources[i],AL_LOOPING,bLp?AL_TRUE:AL_FALSE);
    }
}

// ----------------------------------------------------------------------------
void ofSoundPlayerExtended::setMultiPlay(bool bMp){
    if(isStreaming && bMp){
        ofLog(OF_LOG_WARNING,"ofSoundPlayerExtended: no support for multiplay streams by now");
        return;
    }
    bMultiPlay = bMp;		// be careful with this...
    if(sources.empty()) return;
    //	if(bMultiPlay){
    ofAddListener(ofEvents().update,this,&ofSoundPlayerExtended::update);
    //	}else{
    //		ofRemoveListener(ofEvents().update,this,&ofSoundPlayerExtended::update);
    //	}
}

// ----------------------------------------------------------------------------
void ofSoundPlayerExtended::play(){
    
    // if the sound is set to multiplay, then create new sources,
    // do not multiplay on loop or we won't be able to stop it
    if (bMultiPlay && !bLoop){
        sources.resize(sources.size()+channels);
        alGenSources(channels, &sources[sources.size()-channels]);
        if (alGetError() != AL_NO_ERROR){
            ofLog(OF_LOG_ERROR,"ofSoundPlayerExtended: error creating multiplay stereo sources");
            return;
        }
        for(int i=0;i<channels;i++){
            alSourcei (sources[sources.size()-channels+i], AL_BUFFER,   buffers[i]   );
            // only stereo panning
            if(i==0){
                float pos[3] = {-1,0,0};
                alSourcefv(sources[sources.size()-channels+i],AL_POSITION,pos);
            }else{
                float pos[3] = {1,0,0};
                alSourcefv(sources[sources.size()-channels+i],AL_POSITION,pos);
            }
            alSourcef (sources[sources.size()-channels+i], AL_ROLLOFF_FACTOR,  0.0);
            alSourcei (sources[sources.size()-channels+i], AL_SOURCE_RELATIVE, AL_TRUE);
        }
        
        if (alGetError() != AL_NO_ERROR){
            ofLog(OF_LOG_ERROR,"ofSoundPlayerExtended: error assigning multiplay buffers");
            return;
        }
    }
    alSourcePlayv(channels,&sources[sources.size()-channels]);
    
    //	if(bMultiPlay){
    ofAddListener(ofEvents().update,this,&ofSoundPlayerExtended::update);
    //	}
    if(isStreaming){
        setPosition(0);
        stream_end = false;
        startThread();
    }
    
}

// ----------------------------------------------------------------------------
void ofSoundPlayerExtended::stop(){
    alSourceStopv(channels,&sources[sources.size()-channels]);
}



// ----------------------------------------------------------------------------
vector<float>& ofSoundPlayerExtended::getCurrentBuffer(int _size)
{
    if(int(currentBuffer.size()) != _size)
    {
        currentBuffer.resize(_size);
    }
   	currentBuffer.assign(currentBuffer.size(),0);
    
    int pos;
    for(int k = 0; k < int(sources.size())/channels; ++k)
    {
        alGetSourcei(sources[k*channels],AL_SAMPLE_OFFSET,&pos);
        for(int i = 0; i < channels; ++i)
        {
            for(int j = 0; j < _size; ++j)
            {
                if(pos+j<(int)buffer.size())
                {
                    currentBuffer[j] += float(buffer[(pos+j)*channels+i])/65534.0f;
                }
                else
                {
                    currentBuffer[j] = 0;
                }
            }
        }
    }
    return currentBuffer;
}

//-----------------------------------------------------------
//ofxAA
ofSoundBuffer& ofSoundPlayerExtended::getCurrentSoundBuffer(int _size){
    
    if(currentSoundBuffer.getNumChannels()!= channels){
        ofLogError()<<"ofSoundPlayerExtended: currentSoundBuffer incorrect NumChannels";
    }
    if(currentSoundBuffer.getSampleRate()!= samplerate){
        ofLogError()<<"ofSoundPlayerExtended: currentSoundBuffer incorrect Sample Rate";
    }
    if(channelSoundBuffer.getSampleRate()!= samplerate){
        ofLogError()<<"ofSoundPlayerExtended: channelSoundBuffer incorrect Sample Rate";
    }
    
    //-----------
    
    for (int i=0; i<channels; i++){
        channelSoundBuffer.copyFrom( getCurrentBufferForChannel(_size, i), 1, samplerate);
        currentSoundBuffer.setChannel(channelSoundBuffer, i);
    }
    
    return currentSoundBuffer;
    
}
//-----------------------------------------------------------
ofSoundBuffer&  ofSoundPlayerExtended::getCurrentSoundBufferMono(int _size){
    
    
    if(channelSoundBuffer.getSampleRate()!= samplerate){
        ofLogError()<<"ofSoundPlayerExtended: channelSoundBuffer incorrect Sample Rate";
    }
    
    //-----------
    
    channelSoundBuffer.copyFrom( getCurrentBuffer(_size), 1, samplerate);
    
    return channelSoundBuffer;
    
}
//-----------------------------------------------------------
//ofxAA
vector<float>& ofSoundPlayerExtended::getCurrentBufferForChannel(int _size, int channel){
    
    if(int(currentBuffer.size()) != _size)
    {
        currentBuffer.resize(_size);
    }
   	currentBuffer.assign(currentBuffer.size(),0);
    
    int nCh = channel; //channels number starting from 0
    if (nCh >= channels){
        nCh = channels - 1;//limit to file nChannels
        ofLog(OF_LOG_WARNING,"ofSoundPlayerExtended: channel requested exceeds file channels");
    }
    
    int pos;
    for(int k = 0; k < int(sources.size())/channels; ++k)
    {
        alGetSourcei(sources[k*channels],AL_SAMPLE_OFFSET,&pos);
        //for(int i = 0; i < channels; ++i) //avoid channels sumup
        int i = nCh; //use only specified channel
        {
            for(int j = 0; j < _size; ++j)
            {
                if(pos+j<(int)buffer.size())
                {
                    currentBuffer[j] += float(buffer[(pos+j)*channels+i])/65534.0f;
                }
                else
                {
                    currentBuffer[j] = 0;
                }
            }
        }
    }
    return currentBuffer;
}
//-----------------------------------------------------------
ofSoundBuffer& ofSoundPlayerExtended::getSoundBufferForFrame(int _frame, float _fps, int _size){
    
    if(currentSoundBuffer.getNumChannels()!= channels){
        ofLogError()<<"Sound Player: currentSoundBuffer incorrect NumChannels";
    }
    if(currentSoundBuffer.getSampleRate()!= samplerate){
        ofLogError()<<"Sound Player: currentSoundBuffer incorrect Sample Rate";
    }
    if(channelSoundBuffer.getSampleRate()!= samplerate){
        ofLogError()<<"Sound Player: channelSoundBuffer incorrect Sample Rate";
    }
    
    //---------------------------------------------
    
    for (int i=0; i<channels; i++){
        channelSoundBuffer.copyFrom( getBufferForChannelForFrame(_frame, _fps, _size, i), 1, samplerate);
        currentSoundBuffer.setChannel(channelSoundBuffer, i);
    }
    
    return currentSoundBuffer;
    
    
}
//-----------------------------------------------------------
ofSoundBuffer& ofSoundPlayerExtended::getSoundBufferMonoForFrame(int _frame, float _fps, int _size){
    
    if(channelSoundBuffer.getSampleRate()!= samplerate){
        ofLogError()<<"Sound Player: channelSoundBuffer incorrect Sample Rate";
    }
    
    //-----------
    
    channelSoundBuffer.copyFrom( getBufferForFrame(_frame, _fps, _size), 1, samplerate);
    
    return channelSoundBuffer;
    
}
// ----------------------------------------------------------------------------
//ofxAA
vector<float>& ofSoundPlayerExtended::getBufferForChannelForFrame(int _frame, float _fps, int _size, int channel){
    
    
    if(int(currentBuffer.size()) != _size)
    {
        currentBuffer.resize(_size);
    }
   	currentBuffer.assign(currentBuffer.size(),0);
    
    //ok--------------------------------------
    
    int nCh= channel; //channels number starting from 0
    if (nCh >= channels){
        nCh = channels - 1;//limit to file nChannels
        ofLog(OF_LOG_WARNING,"ofOpenALSoundPlayer_TimelineAdditions: channel requested exceeds file channels");
    }
    
    //ok--------------------------------------
    
    int pos = _frame*float(samplerate)/_fps;
    for(int k = 0; k < int(sources.size())/channels; ++k)
    {
        //for(int i = 0; i < channels; ++i) ///avoid channels sumup
        int i = nCh; //use only specified channel
        {
            for(int j = 0; j < _size; ++j)
            {
                if(pos+j<(int)buffer.size())
                {
                    currentBuffer[j] += float(buffer[(pos+j)*channels+i])/65534.0f;
                }
                else
                {
                    currentBuffer[j] = 0;
                }
            }
        }
    }
    return currentBuffer;
    
    
}

//-----------------------------------------------------------

vector<float>& ofSoundPlayerExtended::getBufferForFrame(int _frame, float _fps, int _size)
{
    if(int(currentBuffer.size()) != _size)
    {
        currentBuffer.resize(_size);
    }
   	currentBuffer.assign(currentBuffer.size(),0);
    
    int pos = _frame*float(samplerate)/_fps;
    for(int k = 0; k < int(sources.size())/channels; ++k)
    {
        for(int i = 0; i < channels; ++i)
        {
            for(int j = 0; j < _size; ++j)
            {
                if(pos+j<(int)buffer.size())
                {
                    currentBuffer[j] += float(buffer[(pos+j)*channels+i])/65534.0f;
                }
                else
                {
                    currentBuffer[j] = 0;
                }
            }
        }
    }
    return currentBuffer;
}

#endif



// ----------------------------------------------------------------------------
