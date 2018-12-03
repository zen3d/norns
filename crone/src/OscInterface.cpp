

//
// Created by ezra on 11/4/18.
//

#include <utility>
#include <thread>
#include <boost/format.hpp>

#include "effects/CompressorParams.h"
#include "effects/ReverbParams.h"
#include "softcut/FadeCurves.h"

#include "Commands.h"
#include "OscInterface.h"

using namespace crone;
using softcut::FadeCurves;

/// TODO: softcut trigger/ phase output

bool OscInterface::quitFlag;

std::string OscInterface::port;
lo_server_thread OscInterface::st;
lo_address OscInterface::matronAddress;

std::array<OscInterface::OscMethod, OscInterface::MaxNumMethods> OscInterface::methods;
unsigned int OscInterface::numMethods = 0;

std::unique_ptr<Poll> OscInterface::vuPoll;
std::unique_ptr<Poll> OscInterface::phasePoll;
MixerClient* OscInterface::mixerClient;
SoftCutClient* OscInterface::softCutClient;

OscInterface::OscMethod::OscMethod(string p, string f, OscInterface::Handler h)
        : path(std::move(p)), format(std::move(f)), handler(h) {}


void OscInterface::init(MixerClient *m, SoftCutClient *sc)
{
    quitFlag = false;
    // FIXME: should get port configs from program args or elsewhere
    port = "9999";
#if 1
    matronAddress = lo_address_new("127.0.0.1", "8888");
#else  // testing with SC
    matronAddress = lo_address_new("127.0.0.1", "57120");
#endif

    st = lo_server_thread_new(port.c_str(), handleLoError);
    addServerMethods();

    mixerClient = m;
    softCutClient = sc;

    //--- VU poll
    vuPoll = std::make_unique<Poll>("vu");
    vuPoll->setCallback([](const char* path){
        auto vl = mixerClient->getVuLevels();
        // FIXME: perform exponential scaling here?
        lo_send(matronAddress, path, "ffff",
                vl->absPeakIn[0].load(),
                vl->absPeakIn[1].load(),
                vl->absPeakOut[0].load(),
                vl->absPeakOut[1].load());
        vl->clear();
    });
    vuPoll->setPeriod(50);

    //--- softcut phase poll
    phasePoll = std::make_unique<Poll>("softcut/phase");
    phasePoll->setCallback([](const char* path) {
        for (int i=0; i<softCutClient->getNumVoices(); ++i) {
            if(softCutClient->checkVoiceQuantPhase(i)) {
                lo_send(matronAddress, path, "if", i, softCutClient->getQuantPhase(i));
            }
        }
    });
    phasePoll->setPeriod(1);


    //--- TODO: softcut trigger poll?

    lo_server_thread_start(st);
}


void OscInterface::addServerMethod(const char* path, const char* format, Handler handler) {
    OscMethod m(path, format, handler);
    methods[numMethods] = m;
    lo_server_thread_add_method(st, path, format,
                                [] (const char *path,
                                    const char *types,
                                    lo_arg **argv,
                                    int argc,
                                    lo_message msg,
                                    void *data)
                                    -> int
                                {
                                    (void) path;
                                    (void) types;
                                    (void) msg;
                                    auto pm = static_cast<OscMethod*>(data);
                                    std::cerr << "osc rx: " << path << std::endl;
                                    pm->handler(argv, argc);
                                    return 0;
                                }, &(methods[numMethods]));
    numMethods++;
}


void OscInterface::addServerMethods() {
    addServerMethod("/hello", "", [](lo_arg **argv, int argc) {
        (void)argv; (void)argc;
        std::cout << "hello" << std::endl;
    });

    addServerMethod("/goodbye", "", [](lo_arg **argv, int argc) {
        (void)argv; (void)argc;
        std::cout << "goodbye" << std::endl;
        OscInterface::quitFlag = true;
    });

    addServerMethod("/quit", "", [](lo_arg **argv, int argc) {
        (void)argv; (void)argc;
        OscInterface::quitFlag = true;
    });


    //---------------------------
    //--- mixer polls

    addServerMethod("/poll/start/vu", "", [](lo_arg **argv, int argc) {
        (void)argv; (void)argc;
        vuPoll->start();
    });

    addServerMethod("/poll/stop/vu", "", [](lo_arg **argv, int argc) {
        (void)argv; (void)argc;
        vuPoll->stop();
    });



    //--------------------------
    //--- levels
    addServerMethod("/set/level/adc", "f", [](lo_arg **argv, int argc) {
        if(argc<1) { return; }
        Commands::mixerCommands.post(Commands::Id::SET_LEVEL_ADC, argv[0]->f);
    });

    addServerMethod("/set/level/dac", "f", [](lo_arg **argv, int argc) {
        if(argc<1) { return; }
        Commands::mixerCommands.post(Commands::Id::SET_LEVEL_DAC, argv[0]->f);
    });

    addServerMethod("/set/level/ext", "f", [](lo_arg **argv, int argc) {
        if(argc<1) { return; }
        Commands::mixerCommands.post(Commands::Id::SET_LEVEL_EXT, argv[0]->f);
    });

    addServerMethod("/set/level/ext_aux", "f", [](lo_arg **argv, int argc) {
        if(argc<1) { return; }
        Commands::mixerCommands.post(Commands::Id::SET_LEVEL_EXT_AUX, argv[0]->f);
    });

    addServerMethod("/set/level/aux_dac", "f", [](lo_arg **argv, int argc) {
        if(argc<1) { return; }
        Commands::mixerCommands.post(Commands::Id::SET_LEVEL_AUX_DAC, argv[0]->f);
    });

    addServerMethod("/set/level/monitor", "f", [](lo_arg **argv, int argc) {
        if(argc<1) { return; }
        Commands::mixerCommands.post(Commands::Id::SET_LEVEL_MONITOR, argv[0]->f);
    });

    addServerMethod("/set/level/monitor_mix", "if", [](lo_arg **argv, int argc) {
        if(argc<2) { return; }
        Commands::mixerCommands.post(Commands::Id::SET_LEVEL_MONITOR_MIX, argv[0]->i, argv[1]->f);
    });

    addServerMethod("/set/level/monitor_aux", "f", [](lo_arg **argv, int argc) {
        if(argc<1) { return; }
        Commands::mixerCommands.post(Commands::Id::SET_LEVEL_MONITOR_AUX, argv[0]->f);
    });

    addServerMethod("/set/level/ins_mix", "f", [](lo_arg **argv, int argc) {
        if(argc<1) { return; }
        Commands::mixerCommands.post(Commands::Id::SET_LEVEL_INS_MIX, argv[0]->f);
    });


    // toggle enabled
    addServerMethod("/set/enabled/compressor", "f", [](lo_arg **argv, int argc) {
        if(argc<1) { return; }
        Commands::mixerCommands.post(Commands::Id::SET_ENABLED_COMPRESSOR, argv[0]->f);
    });

    addServerMethod("/set/enabled/reverb", "f", [](lo_arg **argv, int argc) {
        if(argc<1) { return; }
        Commands::mixerCommands.post(Commands::Id::SET_ENABLED_REVERB, argv[0]->f);
    });

    //-------------------------
    //-- compressor params

    addServerMethod("/set/param/compressor/ratio", "f", [](lo_arg **argv, int argc) {
        if(argc<1) { return; }
        Commands::mixerCommands.post(Commands::Id::SET_PARAM_COMPRESSOR, CompressorParam::RATIO, argv[0]->f);
    });

    addServerMethod("/set/param/compressor/threshold", "f", [](lo_arg **argv, int argc) {
        if(argc<1) { return; }
        Commands::mixerCommands.post(Commands::Id::SET_PARAM_COMPRESSOR, CompressorParam::THRESHOLD, argv[0]->f);
    });

    addServerMethod("/set/param/compressor/attack", "f", [](lo_arg **argv, int argc) {
        if(argc<1) { return; }
        Commands::mixerCommands.post(Commands::Id::SET_PARAM_COMPRESSOR, CompressorParam::ATTACK, argv[0]->f);
    });

    addServerMethod("/set/param/compressor/release", "f", [](lo_arg **argv, int argc) {
        if(argc<1) { return; }
        Commands::mixerCommands.post(Commands::Id::SET_PARAM_COMPRESSOR, CompressorParam::RELEASE, argv[0]->f);
    });

    addServerMethod("/set/param/compressor/gain_pre", "f", [](lo_arg **argv, int argc) {
        if(argc<1) { return; }
        Commands::mixerCommands.post(Commands::Id::SET_PARAM_COMPRESSOR, CompressorParam::GAIN_PRE, argv[0]->f);
    });

    addServerMethod("/set/param/compressor/gain_post", "f", [](lo_arg **argv, int argc) {
        if(argc<1) { return; }
        Commands::mixerCommands.post(Commands::Id::SET_PARAM_COMPRESSOR, CompressorParam::GAIN_POST, argv[0]->f);
    });


    //--------------------------
    //-- reverb params

    addServerMethod("/set/param/reverb/pre_del", "f", [](lo_arg **argv, int argc) {
        if(argc<1) { return; }
        Commands::mixerCommands.post(Commands::Id::SET_PARAM_REVERB, ReverbParam::PRE_DEL, argv[0]->f);
    });

    addServerMethod("/set/param/reverb/lf_fc", "f", [](lo_arg **argv, int argc) {
        if(argc<1) { return; }
        Commands::mixerCommands.post(Commands::Id::SET_PARAM_REVERB, ReverbParam::LF_FC, argv[0]->f);
    });

    addServerMethod("/set/param/reverb/low_rt60", "f", [](lo_arg **argv, int argc) {
        if(argc<1) { return; }
        Commands::mixerCommands.post(Commands::Id::SET_PARAM_REVERB, ReverbParam::LOW_RT60, argv[0]->f);
    });

    addServerMethod("/set/param/reverb/mid_rt60", "f", [](lo_arg **argv, int argc) {
        if(argc<1) { return; }
        Commands::mixerCommands.post(Commands::Id::SET_PARAM_REVERB, ReverbParam::MID_RT60, argv[0]->f);
    });

    addServerMethod("/set/param/reverb/hf_damp", "f", [](lo_arg **argv, int argc) {
        if(argc<1) { return; }
        Commands::mixerCommands.post(Commands::Id::SET_PARAM_REVERB, ReverbParam::HF_DAMP, argv[0]->f);
    });


    //--------------------------------
    //-- softcut routing

    addServerMethod("/set/enabled/cut", "if", [](lo_arg **argv, int argc) {
        if(argc<2) { return; }
        Commands::softcutCommands.post(Commands::Id::SET_ENABLED_CUT, argv[0]->i, argv[1]->f);
    });

    addServerMethod("/set/level/cut", "if", [](lo_arg **argv, int argc) {
        if(argc<2) { return; }
        Commands::softcutCommands.post(Commands::Id::SET_LEVEL_CUT, argv[0]->i, argv[1]->f);
    });

    addServerMethod("/set/pan/cut", "if", [](lo_arg **argv, int argc) {
        if(argc<2) { return; }
        Commands::softcutCommands.post(Commands::Id::SET_PAN_CUT, argv[0]->i, argv[1]->f);
    });


    addServerMethod("/set/level/adc_cut", "f", [](lo_arg **argv, int argc) {
        if(argc<1) { return; }
        Commands::mixerCommands.post(Commands::Id::SET_LEVEL_ADC_CUT, argv[0]->f);
    });

    addServerMethod("/set/level/ext_cut", "f", [](lo_arg **argv, int argc) {
        if(argc<1) { return; }
        Commands::mixerCommands.post(Commands::Id::SET_LEVEL_EXT_CUT, argv[0]->f);

    });

    addServerMethod("/set/level/cut_aux", "f", [](lo_arg **argv, int argc) {
        if(argc<1) { return; }
        Commands::mixerCommands.post(Commands::Id::SET_LEVEL_CUT_AUX, argv[0]->f);
    });


    //--- NB: these are handled by the softcut command queue,
    // because their corresponding mix points are processed by the softcut client.

    // input channel -> voice levels
    addServerMethod("/set/level/in_cut", "iif", [](lo_arg **argv, int argc) {
        if(argc<3) { return; }
        Commands::softcutCommands.post(Commands::Id::SET_LEVEL_IN_CUT, argv[0]->i, argv[1]->i, argv[2]->f);
    });


    // voice ->  voice levels
    addServerMethod("/set/level/cut_cut", "iif", [](lo_arg **argv, int argc) {
        if(argc<3) { return; }
        Commands::softcutCommands.post(Commands::Id::SET_LEVEL_CUT_CUT, argv[0]->i, argv[1]->i, argv[2]->f);
    });


    //--------------------------------
    //-- softcut params


    addServerMethod("/set/param/cut/rate", "if", [](lo_arg **argv, int argc) {
        if(argc<2) { return; }
        Commands::softcutCommands.post(Commands::Id::SET_CUT_RATE, argv[0]->i, argv[1]->f);
    });

    addServerMethod("/set/param/cut/loop_start", "if", [](lo_arg **argv, int argc) {
        if(argc<2) { return; }
        Commands::softcutCommands.post(Commands::Id::SET_CUT_LOOP_START, argv[0]->i, argv[1]->f);
    });

    addServerMethod("/set/param/cut/loop_end", "if", [](lo_arg **argv, int argc) {
        if(argc<2) { return; }
        Commands::softcutCommands.post(Commands::Id::SET_CUT_LOOP_END, argv[0]->i, argv[1]->f);
    });

    addServerMethod("/set/param/cut/loop_flag", "if", [](lo_arg **argv, int argc) {
        if(argc<2) { return; }
        Commands::softcutCommands.post(Commands::Id::SET_CUT_LOOP_FLAG, argv[0]->i, argv[1]->f);
    });

    addServerMethod("/set/param/cut/fade_time", "if", [](lo_arg **argv, int argc) {
        if(argc<2) { return; }
        Commands::softcutCommands.post(Commands::Id::SET_CUT_FADE_TIME, argv[0]->i, argv[1]->f);
    });

    addServerMethod("/set/param/cut/rec_level", "if", [](lo_arg **argv, int argc) {
        if(argc<2) { return; }
        Commands::softcutCommands.post(Commands::Id::SET_CUT_REC_LEVEL, argv[0]->i, argv[1]->f);
    });

    addServerMethod("/set/param/cut/pre_level", "if", [](lo_arg **argv, int argc) {
        if(argc<2) { return; }
        Commands::softcutCommands.post(Commands::Id::SET_CUT_PRE_LEVEL, argv[0]->i, argv[1]->f);
    });

    addServerMethod("/set/param/cut/rec_flag", "if", [](lo_arg **argv, int argc) {
        if(argc<2) { return; }
        Commands::softcutCommands.post(Commands::Id::SET_CUT_REC_FLAG, argv[0]->i, argv[1]->f);
    });

    addServerMethod("/set/param/cut/rec_offset", "if", [](lo_arg **argv, int argc) {
        if(argc<2) { return; }
        Commands::softcutCommands.post(Commands::Id::SET_CUT_REC_OFFSET, argv[0]->i, argv[1]->f);
    });

    addServerMethod("/set/param/cut/position", "if", [](lo_arg **argv, int argc) {
        if(argc<2) { return; }
        Commands::softcutCommands.post(Commands::Id::SET_CUT_POSITION, argv[0]->i, argv[1]->f);
    });

    addServerMethod("/set/param/cut/filter_fc", "if", [](lo_arg **argv, int argc) {
        if(argc<2) { return; }
        Commands::softcutCommands.post(Commands::Id::SET_CUT_FILTER_FC, argv[0]->i, argv[1]->f);
    });

    addServerMethod("/set/param/cut/filter_fc_mod", "if", [](lo_arg **argv, int argc) {
        if(argc<2) { return; }
        Commands::softcutCommands.post(Commands::Id::SET_CUT_FILTER_FC_MOD, argv[0]->i, argv[1]->f);
    });

    addServerMethod("/set/param/cut/filter_rq", "if", [](lo_arg **argv, int argc) {
        if(argc<2) { return; }
        Commands::softcutCommands.post(Commands::Id::SET_CUT_FILTER_RQ, argv[0]->i, argv[1]->f);
    });

    addServerMethod("/set/param/cut/filter_lp", "if", [](lo_arg **argv, int argc) {
        if(argc<2) { return; }
        Commands::softcutCommands.post(Commands::Id::SET_CUT_FILTER_LP, argv[0]->i, argv[1]->f);
    });

    addServerMethod("/set/param/cut/filter_hp", "if", [](lo_arg **argv, int argc) {
        if(argc<2) { return; }
        Commands::softcutCommands.post(Commands::Id::SET_CUT_FILTER_HP, argv[0]->i, argv[1]->f);
    });

    addServerMethod("/set/param/cut/filter_bp", "if", [](lo_arg **argv, int argc) {
        if(argc<2) { return; }
        Commands::softcutCommands.post(Commands::Id::SET_CUT_FILTER_BP, argv[0]->i, argv[1]->f);
    });

    addServerMethod("/set/param/cut/filter_br", "if", [](lo_arg **argv, int argc) {
        if(argc<2) { return; }
        Commands::softcutCommands.post(Commands::Id::SET_CUT_FILTER_BR, argv[0]->i, argv[1]->f);
    });

    addServerMethod("/set/param/cut/filter_dry", "if", [](lo_arg **argv, int argc) {
        if(argc<2) { return; }
        Commands::softcutCommands.post(Commands::Id::SET_CUT_FILTER_DRY, argv[0]->i, argv[1]->f);
    });


    //////////////////////////////////////////////////////////
    /// FIXME: these fade calculation methods create worker threads,
    /// so as not to hold up either OSC server or audio processing.
    /// this is probably not be the best place to do that;
    /// it also doesn't entirely rule out glitches during fades.
    /// perhaps these parameters should not be modulatable at all.

    addServerMethod("/set/param/cut/pre_fade_window", "if", [](lo_arg **argv, int argc) {
        if(argc<1) { return; }
        float x = argv[0]->f;
        auto t = std::thread([x] {
            FadeCurves::setPreWindowRatio(x);
        });
        t.detach();
    });

    addServerMethod("/set/param/cut/rec_fade_delay", "if", [](lo_arg **argv, int argc) {
        if(argc<1) { return; }
        float x = argv[0]->f;
        auto t = std::thread([x] {
            FadeCurves::setRecDelayRatio(x);
        });
        t.detach();
    });

    addServerMethod("/set/param/cut/pre_fade_shape", "if", [](lo_arg **argv, int argc) {
        if(argc<1) { return; }
        float x = argv[0]->f;
        auto t = std::thread([x] {
            FadeCurves::setPreShape(static_cast<FadeCurves::Shape>(x));
        });
        t.detach();
    });

    addServerMethod("/set/param/cut/rec_fade_shape", "if", [](lo_arg **argv, int argc) {
        if(argc<1) { return; }
        float x = argv[0]->f;
        auto t = std::thread([x] {
            FadeCurves::setRecShape(static_cast<FadeCurves::Shape>(x));
        });
        t.detach();
    });
    //////////////////
    ///////////////////

    addServerMethod("/set/param/cut/level_slew_time", "if", [](lo_arg **argv, int argc) {
        if (argc<2) { return; }
        Commands::softcutCommands.post(Commands::Id::SET_CUT_LEVEL_SLEW_TIME, argv[0]->i, argv[1]->f);
    });

    addServerMethod("/set/param/cut/rate_slew_time", "if", [](lo_arg **argv, int argc) {
        if (argc<2) { return; }
        Commands::softcutCommands.post(Commands::Id::SET_CUT_RATE_SLEW_TIME, argv[0]->i, argv[1]->f);
    });


    //-------------------------------
    //--- softcut buffer manipulation

    // FIXME: hrm, our system doesn't allow variable argument count. maybe need to make multiple methods
    addServerMethod("/softcut/buffer/read", "sfffi", [](lo_arg **argv, int argc) {
        float startSrc = 0.f;
        float startDst = 0.f;
        float dur = -1.f;
        int channel=0;
        if (argc < 1) {
            std::cerr << "/softcut/buffer/read requires at least one argument (file path)" << std::endl;
            return;
        }
        if (argc > 1) {
            startSrc = argv[1]->f;
        }
        if (argc > 2) {
            startDst = argv[2]->f;
        }
        if (argc > 3) {
            dur = argv[3]->f;
        }
        if (argc > 4) {
            channel = argv[4]->i;
        }
        const char *str = &argv[0]->s;
        softCutClient->loadFile(str, startSrc, startDst, dur, channel);
    });

    addServerMethod("/softcut/buffer/clear", "ff", [](lo_arg **argv, int argc) {
        if (argc < 2) {
            return;
        }
        softCutClient->clearBuffer(argv[0]->f, argv[1]->f);
    });

    // FIXME: does it even work to do this?
    addServerMethod("/softcut/buffer/clear", "", [](lo_arg **argv, int argc) {
        (void)argc; (void)argv;
        softCutClient->clearBuffer();
    });


    //---------------------
    //--- softcut polls

    addServerMethod("/set/param/cut/phase_quant", "if", [](lo_arg **argv, int argc) {
        if (argc<2) { return; }
        softCutClient->setPhaseQuant(argv[0]->i, argv[1]->f);
    });

    addServerMethod("/poll/start/cut/phase", "", [](lo_arg **argv, int argc) {
        (void)argv; (void)argc;
        phasePoll->start();
    });

    addServerMethod("/poll/stop/cut/phase", "", [](lo_arg **argv, int argc) {
        (void)argv; (void)argc;
        phasePoll->stop();
    });


    //------------------------
    //--- tape control

    addServerMethod("/tape/record/open", "s", [](lo_arg **argv, int argc) {
        if (argc<1) { return; }
        mixerClient->openTapeRecord(&argv[0]->s);
    });

    addServerMethod("/tape/record/start", "", [](lo_arg **argv, int argc) {
        (void) argv; (void) argc;
        mixerClient->startTapeRecord();
    });

    addServerMethod("/tape/record/stop", "", [](lo_arg **argv, int argc) {
        (void) argv; (void) argc;
        mixerClient->stopTapeRecord();
    });


    // TODO: tape playback

}

void OscInterface::printServerMethods() {
    using std::cout;
    using std::endl;
    using std::string;
    using boost::format;
    cout << "osc methods: " << endl;
    for (unsigned int i=0; i<numMethods; ++i) {
        cout << format(" %1% [%2%]") % methods[i].path % methods[i].format << endl;
    }
}

void OscInterface::deinit() {
    lo_address_free(matronAddress);
}

