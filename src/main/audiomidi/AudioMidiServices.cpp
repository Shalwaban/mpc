#include "AudioMidiServices.hpp"

// mpc
#include <StartUp.hpp>
#include <Mpc.hpp>
#include <audiomidi/DirectToDiskSettings.hpp>
#include <audiomidi/ExportAudioProcessAdapter.hpp>
#include <audiomidi/MpcMidiPorts.hpp>
#include <ui/sampler/SamplerGui.hpp>
#include <ui/sampler/MixerSetupGui.hpp>
#include <nvram/NvRam.hpp>
#include <nvram/AudioMidiConfig.hpp>
#include <sampler/Sampler.hpp>
#include <sequencer/Sequencer.hpp>
#include <mpc/MpcVoice.hpp>
#include <mpc/MpcBasicSoundPlayerChannel.hpp>
#include <mpc/MpcBasicSoundPlayerControls.hpp>
#include <mpc/MpcFaderControl.hpp>
#include <mpc/MpcMixerControls.hpp>
#include <mpc/MpcMultiMidiSynth.hpp>
#include <mpc/MpcMultiSynthControls.hpp>
#include <mpc/MpcSoundPlayerChannel.hpp>
#include <mpc/MpcSoundPlayerControls.hpp>

// ctoot
#include <audio/core/AudioFormat.hpp>
#include <audio/core/AudioControlsChain.hpp>
#include <audio/core/AudioProcess.hpp>
#include <audio/core/ChannelFormat.hpp>

#include <audio/mixer/AudioMixer.hpp>
#include <audio/mixer/AudioMixerBus.hpp>
#include <audio/mixer/AudioMixerStrip.hpp>
#include <audio/mixer/MixerControlsFactory.hpp>
#include <audio/mixer/MixerControls.hpp>

#include <audio/server/CompoundAudioClient.hpp>
#include <audio/server/IOAudioProcess.hpp>
#include <audio/server/NonRealTimeAudioServer.hpp>
#include <audio/server/UnrealAudioServer.hpp>
#include <audio/server/ExternalAudioServer.hpp>

#include <audio/system/MixerConnectedAudioSystem.hpp>
#include <audio/system/AudioDevice.hpp>
#include <audio/system/AudioOutput.hpp>

#include <control/CompoundControl.hpp>
#include <control/Control.hpp>

#include <midi/core/MidiSystem.hpp>
#include <midi/core/ConnectedMidiSystem.hpp>
#include <midi/core/DefaultConnectedMidiSystem.hpp>

#include <synth/MidiChannel.hpp>
#include <synth/MidiSynth.hpp>
#include <synth/SynthChannel.hpp>
#include <synth/SynthChannelControls.hpp>
#include <synth/SynthRack.hpp>
#include <synth/SynthRackControls.hpp>

#include <audio/reverb/BarrControls.hpp>
#include <audio/reverb/BarrProcess.hpp>

#include <audio/core/AudioServices.hpp>
#include <audio/spi/AudioServiceProvider.hpp>

#include <synth/synths/multi/MultiSynthServiceProvider.hpp>
#include <synth/SynthServices.hpp>
#include <synth/SynthChannelServices.hpp>

// moduru
#include <file/File.hpp>
#include <file/FileUtil.hpp>
#include <lang/StrUtil.hpp>

#include <mpc/MpcSampler.hpp>
#include <mpc/MpcMixerSetupGui.hpp>

#include <Logger.hpp>

// stl
#include <cmath>
#include <string>

using namespace mpc::audiomidi;
using namespace ctoot::audio::server;
using namespace ctoot::audio::core;
using namespace std;

AudioMidiServices::AudioMidiServices(mpc::Mpc* mpc)
{
	this->mpc = mpc;
	frameSeq = make_shared<mpc::sequencer::FrameSeq>(mpc);
	disabled = true;
	AudioServices::scan();
	ctoot::synth::SynthServices::scan();
	ctoot::synth::SynthChannelServices::scan();
}

void AudioMidiServices::start(const int sampleRate, const int inputCount, const int outputCount) {
	format = make_shared<AudioFormat>(sampleRate, 16, 2, true, false);
	setupMidi();
	server = make_shared<ExternalAudioServer>();

	/*
	if (mode.compare("rtaudio") == 0) {
	}
	else if (mode.compare("unreal") == 0) {
		server = make_shared<UnrealAudioServer>();
	}
	*/
	server->setSampleRate(sampleRate);
	offlineServer = make_shared<NonRealTimeAudioServer>(server);
	MLOG("AMS start, samplerate " + std::to_string(offlineServer->getSampleRate()));
	setupMixer();

	inputProcesses = vector<IOAudioProcess*>(inputCount <= 1 ? inputCount : 1);
	outputProcesses = vector<IOAudioProcess*>(outputCount <= 5 ? outputCount : 5);

	for (auto& p : inputProcesses)
		p = nullptr;

	for (auto& p : outputProcesses)
		p = nullptr;

	for (int i = 0; i < inputProcesses.size(); i++) {
		inputProcesses[i] = server->openAudioInput(getInputNames()[i], "mpc_in" + to_string(i));
	}

	for (int i = 0; i < outputProcesses.size(); i++) {
		outputProcesses[i] = server->openAudioOutput(getOutputNames()[i], "mpc_out" + to_string(i));
	}

	createSynth(sampleRate);
	if (oldPrograms.size() != 0) {
		for (int i = 0; i < 4; i++) {
			mpc->getDrum(i)->setProgram(oldPrograms[i]);
		}
	}
	connectVoices();
	mpcMidiPorts = make_shared<MpcMidiPorts>(mpc);
	mpcMidiPorts->setMidiIn1(-1);
	mpcMidiPorts->setMidiIn2(-1);
	mpcMidiPorts->setMidiOutA(-1);
	mpcMidiPorts->setMidiOutB(-1);
	auto sampler = mpc->getSampler().lock();
	if (inputProcesses.size() >= 1) {
		sampler->setActiveInput(inputProcesses[mpc->getUis().lock()->getSamplerGui()->getInput()]);
		mixer->getStrip("66").lock()->setInputProcess(sampler->getAudioOutputs()[0]);
	}
	initializeDiskWriter();
	cac = make_shared<CompoundAudioClient>();
	cac->add(frameSeq.get());
	cac->add(mixer.get());
	//cac->add(midiSystem.get());
	sampler->setSampleRate(sampleRate);
	cac->add(sampler.get());
	offlineServer->setWeakPtr(offlineServer);
	offlineServer->setClient(cac);
	offlineServer->resizeBuffers(8192*4);
	offlineServer->start();
	//disabled = false;
	MLOG("audio midi services started");
}

void AudioMidiServices::setupMidi()
{
	midiSystem = make_shared<ctoot::midi::core::DefaultConnectedMidiSystem>();
}

NonRealTimeAudioServer* AudioMidiServices::getOfflineServer() {
	return offlineServer.get();
}

weak_ptr<AudioServer> AudioMidiServices::getAudioServer() {
	return offlineServer;
}

void AudioMidiServices::setupMixer()
{
	mixerControls = make_shared<ctoot::mpc::MpcMixerControls>("MpcMixerControls", 1.f);
	
	// AUX#1 - #4 represent ASSIGNABLE MIX OUT 1/2, 3/4, 5/6 and 7/8
	mixerControls->createAuxBusControls("AUX#1", ChannelFormat::STEREO());
	mixerControls->createAuxBusControls("AUX#2", ChannelFormat::STEREO());
	mixerControls->createAuxBusControls("AUX#3", ChannelFormat::STEREO());
	mixerControls->createAuxBusControls("AUX#4", ChannelFormat::STEREO());
	
	// FX#1 Represents the MPC2000XL's only FX send bus
	mixerControls->createFxBusControls("FX#1", ChannelFormat::STEREO());
	int nReturns = 1;
	
	// L/R represents STEREO OUT L/R
	ctoot::audio::mixer::MixerControlsFactory::createBusStrips(dynamic_pointer_cast<ctoot::audio::mixer::MixerControls>(mixerControls), "L-R", ChannelFormat::STEREO(), nReturns);
	
	/*
	* There are 32 voices. Each voice has one channel for mixing to STEREO OUT L/R, and one channel for mixing to an ASSIGNABLE MIX OUT. These are strips 1-64.
	* There's one channel for the MpcBasicSoundPlayerChannel, which plays the metronome, preview and playX sounds. This is strip 65.
	* Finally there's one channel to monitor sampler input. This is strip 66. Hence nMixerChans = 66;
	*/
	int nMixerChans = 66;
	ctoot::audio::mixer::MixerControlsFactory::createChannelStrips(mixerControls, nMixerChans);
	mixer = make_shared<ctoot::audio::mixer::AudioMixer>(mixerControls, offlineServer);
	audioSystem = make_shared<ctoot::audio::system::MixerConnectedAudioSystem>(mixer);
	audioSystem->setAutoConnect(false);
	setMasterLevel(nvram::NvRam::getMasterLevel());
	setRecordLevel(nvram::NvRam::getRecordLevel());
	setAssignableMixOutLevels();
	setupFX();
}

void AudioMidiServices::setupFX() {
	auto acs = mixer->getMixerControls().lock()->getStripControls("FX#1").lock();
	acs->insert("ctoot::audio::reverb::BarrControls", "Main");
	//acs->insert("class ctoot::audio::delay::WowFlutterControls", "Main");
	//acs->insert("class ctoot::audio::delay::ModulatedDelayControls", "Main");
	//acs->insert("class ctoot::audio::delay::MultiTapDelayStereoControls", "Main"); // doesn't seem to work at all
	//acs->insert("class ctoot::audio::delay::TempoDelayControls", "Main");
	//acs->insert("class ctoot::audio::delay::PhaserControls", "Main");
	//acs->insert("class ctoot::audio::delay::CabMicingControls", "Main"); doesn't seem to work at all
	//auto controls = AudioServices::getAvailableControls();
	//MLOG("Available controls:");
	//MLOG(controls);
}

void AudioMidiServices::setMasterLevel(int i)
{
	auto sc = mixer->getMixerControls().lock()->getStripControls("L-R").lock();
	auto cc = dynamic_pointer_cast<ctoot::control::CompoundControl>(sc->find("Main").lock());
	dynamic_pointer_cast<ctoot::mpc::MpcFaderControl>(cc->find("Level").lock())->setValue(i);
}

int AudioMidiServices::getMasterLevel() {
	auto sc = mixer->getMixerControls().lock()->getStripControls("L-R").lock();
	auto cc = dynamic_pointer_cast<ctoot::control::CompoundControl>(sc->find("Main").lock());
	auto val = dynamic_pointer_cast<ctoot::mpc::MpcFaderControl>(cc->find("Level").lock())->getValue();
	return (int)(val);
}

void AudioMidiServices::setRecordLevel(int i) {
	mpc->getSampler().lock()->setInputLevel(i);
}

int AudioMidiServices::getRecordLevel() {
	return mpc->getSampler().lock()->getInputLevel();
}

void AudioMidiServices::setAssignableMixOutLevels()
{
	/*
	* We have to make sure the ASSIGNABLE MIX OUTs are audible. They're fixed at value 100.
	*/
	for (auto j = 1; j <= 4; j++) {
		string name = "AUX#" + to_string(j);
		auto sc = mixer->getMixerControls().lock()->getStripControls(name).lock();
		auto cc = dynamic_pointer_cast<ctoot::control::CompoundControl>(sc->find(name).lock());
		dynamic_pointer_cast<ctoot::mpc::MpcFaderControl>(cc->find("Level").lock())->setValue(100);
	}
}

vector<string> AudioMidiServices::getInputNames()
{
	if (!server) return vector<string>{"<disabled>"};
	return server->getAvailableInputNames();
}

vector<string> AudioMidiServices::getOutputNames()
{
	if (!server) return vector<string>{"<disabled>"};
	return server->getAvailableOutputNames();
}

weak_ptr<ctoot::mpc::MpcMultiMidiSynth> AudioMidiServices::getMms()
{
	return mms;
}

void AudioMidiServices::createSynth(int sampleRate)
{
	synthRackControls = make_shared<ctoot::synth::SynthRackControls>(1);
	synthRack = make_shared<ctoot::synth::SynthRack>(synthRackControls, midiSystem, audioSystem);
	msc = make_shared<ctoot::mpc::MpcMultiSynthControls>();
	msc->setSampleRate(sampleRate);
	synthRackControls->setSynthControls(0, msc);
	mms = dynamic_pointer_cast<ctoot::mpc::MpcMultiMidiSynth>(synthRack->getMidiSynth(0).lock());
	auto msGui = mpc->getUis().lock()->getMixerSetupGui();
	for (int i = 0; i < 4; i++) {
		auto m = make_shared<ctoot::mpc::MpcSoundPlayerControls>(mms, dynamic_pointer_cast<ctoot::mpc::MpcSampler>(mpc->getSampler().lock()), i, mixer, server, dynamic_cast<ctoot::mpc::MpcMixerSetupGui*>(msGui));
		msc->setChannelControls(i, m);
		synthChannelControls.push_back(m);
	}
	basicVoice = make_shared<ctoot::mpc::MpcVoice>(65, true, sampleRate);
	auto m = make_shared<ctoot::mpc::MpcBasicSoundPlayerControls>(mpc->getSampler(), mixer, basicVoice);
	msc->setChannelControls(4, m);
	synthChannelControls.push_back(std::move(m));
}

void AudioMidiServices::connectVoices()
{
	mpc->getDrums()[0]->connectVoices();
	mpc->getBasicPlayer()->connectVoice();
}

weak_ptr<MpcMidiPorts> AudioMidiServices::getMidiPorts()
{
	return mpcMidiPorts;
}

void AudioMidiServices::initializeDiskWriter()
{
	auto diskWriter = make_shared<ExportAudioProcessAdapter>(outputProcesses[0], format, "diskwriter");
	mixer->getMainBus()->setOutputProcess(diskWriter);
	exportProcesses.push_back(std::move(diskWriter));

	for (int i = 1; i < outputProcesses.size(); i++) {
		diskWriter = make_shared<ExportAudioProcessAdapter>(outputProcesses[i], format, "diskwriter");
		mixer->getStrip(string("AUX#" + to_string(i))).lock()->setDirectOutputProcess(diskWriter);
		exportProcesses.push_back(std::move(diskWriter));
	}
}

void AudioMidiServices::setDisabled(bool b) {
	disabled = b;
}

void AudioMidiServices::destroyServices()
{
	disabled = true;
	MLOG("Trying to destroy services...");
	offlineServer->stop();
	cac.reset();
	destroyDiskWriter();
	mpc->getSampler().lock()->setActiveInput(nullptr);
	mixer->getStrip("66").lock()->setInputProcess(weak_ptr<AudioProcess>());
	mpcMidiPorts->close();
	mpcMidiPorts.reset();
	closeIO();
	inputProcesses.clear();
	outputProcesses.clear();
	audioSystem->close();
	audioSystem.reset();
	destroySynth();
	mixer->close();
	mixer.reset();
	mixerControls.reset();
	offlineServer->close();
	offlineServer.reset();
	server.reset();
	midiSystem->close();
	midiSystem.reset();
}

void AudioMidiServices::destroySynth() {
	oldPrograms = vector<int>(4);
	for (int i = 0; i < 4; i++) {
		oldPrograms[i] = mpc->getDrum(i)->getProgram();
	}
	for (auto& s : synthChannelControls) {
		s.reset();
	}
	synthChannelControls.clear();
	voices.clear();
	basicVoice.reset();
	msc.reset();

	synthRack->close();
	synthRack.reset();

	synthRackControls.reset();
}

void AudioMidiServices::destroyDiskWriter() {
	exportProcesses.clear();
}

void AudioMidiServices::closeIO()
{
	for (auto j = 0; j < inputProcesses.size(); j++) {
		if (inputProcesses[j] == nullptr) {
			continue;
		}
		server->closeAudioInput(inputProcesses[j]);
	}
	for (auto j = 0; j < outputProcesses.size(); j++) {
		if (outputProcesses[j] == nullptr) {
			continue;
		}
		server->closeAudioOutput(outputProcesses[j]);
	}
}

void AudioMidiServices::prepareBouncing(DirectToDiskSettings* settings)
{
	auto indivFileNames = std::vector<string>{ "L-R", "1-2", "3-4", "5-6", "7-8" };
	string sep = moduru::file::FileUtil::getSeparator();
	for (int i = 0; i < exportProcesses.size(); i++) {
		auto eapa = exportProcesses[i];
		auto file = new moduru::file::File(mpc::StartUp::home + sep + "vMPC" + sep + "recordings" + sep + indivFileNames[i], nullptr);
		eapa->prepare(file, settings->lengthInFrames, settings->sampleRate);
	}
	bouncePrepared = true;
}

void AudioMidiServices::startBouncing()
{
	if (!bouncePrepared)
		return;
	bouncePrepared = false;
	bouncing = true;
	for (auto& eapa : exportProcesses) {
		eapa->start();
	}
}

void AudioMidiServices::stopBouncing()
{
	if (!bouncing) return;

	for (auto& eapa : exportProcesses) {
		eapa->stop();
	}
	for (auto& eapa : exportProcesses) {
		eapa->writeWav();
	}
	mpc->getSequencer().lock()->stop();
	mpc->getLayeredScreen().lock()->openScreen("recordingfinished");
	bouncing = false;
}

weak_ptr<mpc::sequencer::FrameSeq> AudioMidiServices::getFrameSequencer()
{
	return frameSeq;
}

bool AudioMidiServices::isBouncePrepared()
{
	return bouncePrepared;
}

bool AudioMidiServices::isBouncing()
{
	return bouncing;
}

void AudioMidiServices::disable()
{
	disabled = true;
}

bool AudioMidiServices::isDisabled()
{
	return disabled;
}

IOAudioProcess* AudioMidiServices::getAudioInput(int input)
{
	return inputProcesses[input];
}

int AudioMidiServices::getBufferSize()
{
	return server->getOutputLatencyFrames();
}

UnrealAudioServer* AudioMidiServices::getUnrealAudioServer() {
	return dynamic_pointer_cast<UnrealAudioServer>(server).get();
}

ExternalAudioServer* AudioMidiServices::getExternalAudioServer() {
	return dynamic_pointer_cast<ExternalAudioServer>(server).get();
}

AudioMidiServices::~AudioMidiServices() {
}
