#pragma once
#include <fstream>
#include <vector>

using namespace std;

static const int BUFFER_SIZE{ 2048 };
static const int FMT_CHUNK_ID{ 544501094 };
static const int DATA_CHUNK_ID{ 1635017060 };
static const int RIFF_CHUNK_ID{ 1179011410 };
static const int RIFF_TYPE_ID{ 1163280727 };

ofstream ws_init(const string& path) {
    ofstream result(path.c_str(), ios::out | ios::binary);
	return result;
}

void ws_putLE(ofstream& stream, int val, int numBytes)
{
    for (auto b = 0; b < numBytes; b++) {
        stream <<static_cast<char>(val & 255);
        val >>= 8;
    }
}

void ws_writeHeader(ofstream& stream, const int sampleRate) {
	const int numChannels = 2;
	const int validBits = 16;

	auto bytesPerSample = (validBits + 7) / 8;
	auto blockAlign = bytesPerSample * numChannels;

    auto averageBytesPerSecond = sampleRate * numChannels * bytesPerSample;

    ws_putLE(stream, RIFF_CHUNK_ID, 4); // Offset 0
    ws_putLE(stream, 0, 4); // Offset 4. For now we set main chunk size to 0
    ws_putLE(stream, RIFF_TYPE_ID, 4); // Offset 8
    ws_putLE(stream, FMT_CHUNK_ID, 4); // Offset 12
    ws_putLE(stream, 16, 4); // Offset 16
    ws_putLE(stream, 1, 2); // Offset 20
    ws_putLE(stream, numChannels, 2); // Offset 22
    ws_putLE(stream, sampleRate, 4); // Offset 24
    ws_putLE(stream, averageBytesPerSecond, 4); // Offset 28
    ws_putLE(stream, blockAlign, 2); // Offset 32
    ws_putLE(stream, validBits, 2); // Offset 34
    ws_putLE(stream, DATA_CHUNK_ID, 4); // Ofset 36
    ws_putLE(stream, 0, 4); // Offset 40. For now we set data chunk size 0 
}

void ws_write_bytes(ofstream& stream, const vector<char>& bytes) {
    stream.write((char*)(&bytes[0]), bytes.size());
}

void ws_close(ofstream& stream, const int sampleRate, const int frameCount) {
    const int numChannels = 2;
    const int validBits = 16;

    auto bytesPerSample = (validBits + 7) / 8;
    auto blockAlign = bytesPerSample * numChannels;

    auto averageBytesPerSecond = sampleRate * numChannels * bytesPerSample;

    auto dataChunkSize = blockAlign * frameCount;
    auto mainChunkSize = 4 + 8 + 16 + 8 + dataChunkSize;
    
    bool wordAlignAdjust = false;
 
    if (dataChunkSize % 2 == 1) {
        mainChunkSize += 1;
        wordAlignAdjust = true;
    }
    
    if (wordAlignAdjust) {
        stream << 0;
    }

    stream.seekp(4);
    ws_putLE(stream, mainChunkSize, 4); // We correct the main chunk size to reflect final frame count.

    stream.seekp(40);
    ws_putLE(stream, dataChunkSize, 4); // Same here for data chunk size.
    
    stream.close();
}