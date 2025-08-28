#ifndef _WS_Flow_H_
#define _WS_Flow_H_

#include <Adafruit_GFX.h>
#include <Adafruit_NeoMatrix.h>
#include <Adafruit_NeoPixel.h>

#define RGB_Control_PIN   14  

int getCharWidth(char c);
int getStringWidth(const char* str);
void Text_Flow(char* Text);
void Matrix_Init();   
void Matrix_ResetScroll(); // 追加: スクロール位置をリセットして即座に新テキストを右端から表示
// 起動確認の全点灯テスト（指定RGBでduration_msミリ秒点灯→消灯）
void Matrix_BootTest(uint8_t r, uint8_t g, uint8_t b, uint16_t duration_ms);
#endif
