#ifndef MIDIBUFFER_NT_HOST_HPP
#define MIDIBUFFER_NT_HOST_HPP

#include <stdint.h>

namespace midibuffer {
namespace nt_host {

void drawText(int x, int y, const char* text);
void sendMidiByte(uint32_t destination, uint8_t byte0);
void sendMidi2(uint32_t destination, uint8_t byte0, uint8_t byte1);
void sendMidi3(uint32_t destination, uint8_t byte0, uint8_t byte1, uint8_t byte2);

}  // namespace nt_host
}  // namespace midibuffer

#endif
