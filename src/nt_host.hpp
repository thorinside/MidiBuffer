#ifndef MIDIBUFFER_NT_HOST_HPP
#define MIDIBUFFER_NT_HOST_HPP

#include <distingnt/api.h>

#include <stdint.h>

namespace midibuffer {
namespace nt_host {

void drawText(int x, int y, const char* text);
void drawTinyText(int x, int y, const char* text);
void drawShape(_NT_shape shape, int x0, int y0, int x1, int y1,
               int colour = 15);
void sendMidiByte(uint32_t destination, uint8_t byte0,
                  uint64_t dispatchSample = 0);
void sendMidi2(uint32_t destination, uint8_t byte0, uint8_t byte1,
               uint64_t dispatchSample = 0);
void sendMidi3(uint32_t destination, uint8_t byte0, uint8_t byte1,
               uint8_t byte2, uint64_t dispatchSample = 0);

}  // namespace nt_host
}  // namespace midibuffer

#endif
