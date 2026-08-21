cbuffer FadeConstants : register(b0) {
  float enabled;
  float coverage;
};

struct PixelInput { float4 position : SV_Position; };

static const float thresholds[9] = {
  0.0, 0.5, 0.125, 0.625, 0.25, 0.75, 0.875, 0.375, 1.0
};

float4 main(PixelInput input) : SV_Target {
  float dither = 1.0;
  [branch]
  if (enabled > 0.0) {
    int x = ((int)input.position.x) % 3;
    int y = ((int)input.position.y) % 3;
    float threshold = thresholds[x * 3 + y];
    dither = min(max(coverage * 2.0 - threshold, 0.0), 1.0) + 0.333;
  }
  clip(dither - 0.5);
  return float4(1.0, 1.0, 1.0, dither);
}
