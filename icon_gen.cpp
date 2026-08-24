// icon_gen.cpp —— 一次性工具：用 GDI 绘制 “LR” 圆角图标，输出多尺寸 appicon.ico
// 编译：g++ -std=c++17 -O2 -s icon_gen.cpp -o icon_gen.exe -lgdi32 -luser32
// 运行：./icon_gen.exe  （生成同目录 appicon.ico）
#include <windows.h>
#include <cstdio>
#include <vector>
#include <string>

#pragma pack(push,1)
struct ICONDIR { WORD reserved, type, count; };
struct ICONDIRENTRY {
  BYTE width, height, colorCount, reserved;
  WORD planes, bitCount; DWORD bytesInRes, imageOffset;
};
#pragma pack(pop)

struct RGBA { BYTE b, g, r, a; };

// 生成 size×size 的“LR”圆角磁贴位图（navy 底 + 白色 LR，圆角外透明）
static HBITMAP makeIconBitmap(int sz){
  HDC hdc = GetDC(NULL);
  HDC mdc = CreateCompatibleDC(hdc);
  ReleaseDC(NULL, hdc);

  BITMAPINFO bi = {0};
  bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth       = sz;
  bi.bmiHeader.biHeight      = sz;   // 正数 => 顶层向下（top-down）
  bi.bmiHeader.biPlanes      = 1;
  bi.bmiHeader.biBitCount    = 32;
  bi.bmiHeader.biCompression = BI_RGB;

  RGBA* bits = NULL;
  HBITMAP hBmp = CreateDIBSection(mdc, &bi, DIB_RGB_COLORS, (void**)&bits, NULL, 0);
  HBITMAP ob = (HBITMAP)SelectObject(mdc, hBmp);

  // 1) 圆角磁贴背景（navy，不透明）；圆角外保持透明（alpha=0）
  int pad = (int)(sz * 0.05f + 0.5f);
  int rad = (int)(sz * 0.20f + 0.5f);
  int x0 = pad, y0 = pad, x1 = sz - pad, y1 = sz - pad;
  for (int y = 0; y < sz; y++) {
    for (int x = 0; x < sz; x++) {
      int cx = (x < x0 + rad) ? x0 + rad : (x > x1 - rad ? x1 - rad : x);
      int cy = (y < y0 + rad) ? y0 + rad : (y > y1 - rad ? y1 - rad : y);
      bool inside = (x >= x0 && x <= x1 && y >= y0 && y <= y1) &&
                    ((x - cx) * (x - cx) + (y - cy) * (y - cy) <= rad * rad);
      if (inside) {
        RGBA& p = bits[(size_t)y * sz + x];
        p.r = 0x2B; p.g = 0x2B; p.b = 0x3A; p.a = 0xFF;
      }
    }
  }
  // 2) 画 “LR” 白字（仅字形像素覆盖为白，alpha 保持 255）
  LOGFONTW lf = {0};
  lf.lfHeight   = -(int)(sz * 0.60f);
  lf.lfWeight   = FW_BOLD;
  wcscpy_s(lf.lfFaceName, L"Segoe UI");
  HFONT f  = CreateFontIndirectW(&lf);
  HFONT of = (HFONT)SelectObject(mdc, f);
  SetBkMode(mdc, TRANSPARENT);
  SetTextColor(mdc, RGB(255, 255, 255));
  RECT r = {0, 0, sz, sz};
  DrawTextW(mdc, L"LR", -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  SelectObject(mdc, of);
  DeleteObject(f);

  SelectObject(mdc, ob);
  DeleteDC(mdc);
  return hBmp;
}

static bool writeICO(const wchar_t* path, const std::vector<std::pair<int, HBITMAP>>& imgs){
  FILE* f = _wfopen(path, L"wb");
  if (!f) { fwprintf(stderr, L"无法创建 %s\n", path); return false; }
  int N = (int)imgs.size();

  struct Ent { int w; std::vector<BYTE> xorB; int andRow; std::vector<BYTE> andM; int bytes; };
  std::vector<Ent> ents; ents.reserve(N);

  for (auto& pr : imgs) {
    int w = pr.first, h = w;
    HDC hdc = GetDC(NULL); HDC mdc = CreateCompatibleDC(hdc); ReleaseDC(NULL, hdc);
    BITMAPINFO bi = {0};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = h;   // 正数 => 取底层向上（bottom-up），即 ICO XOR 所需顺序
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    std::vector<BYTE> xorB((size_t)w * h * 4);
    GetDIBits(mdc, pr.second, 0, h, xorB.data(), &bi, DIB_RGB_COLORS);
    DeleteDC(mdc);

    int andRow = ((w + 31) / 32) * 4;
    std::vector<BYTE> andM((size_t)andRow * h, 0); // 全 0 => 由 XOR alpha 决定透明
    Ent e; e.w = w; e.xorB = std::move(xorB); e.andRow = andRow; e.andM = std::move(andM);
    e.bytes = 40 + (int)e.xorB.size() + (int)e.andM.size();
    ents.push_back(std::move(e));
  }

  ICONDIR dir = {0, 1, (WORD)N};
  fwrite(&dir, 1, sizeof(ICONDIR), f);
  DWORD off = (DWORD)(sizeof(ICONDIR) + sizeof(ICONDIRENTRY) * N);
  for (int i = 0; i < N; i++) {
    ICONDIRENTRY e;
    e.width       = (BYTE)(ents[i].w >= 256 ? 0 : ents[i].w);
    e.height      = (BYTE)(ents[i].w >= 256 ? 0 : ents[i].w);
    e.colorCount  = 0;
    e.reserved    = 0;
    e.planes      = 1;
    e.bitCount    = 32;
    e.bytesInRes  = (DWORD)ents[i].bytes;
    e.imageOffset = off;
    fwrite(&e, 1, sizeof(ICONDIRENTRY), f);
    off += e.bytesInRes;
  }
  for (int i = 0; i < N; i++) {
    BITMAPINFOHEADER bih = {0};
    bih.biSize        = 40;
    bih.biWidth       = ents[i].w;
    bih.biHeight      = 2 * ents[i].w;  // XOR(上) + AND(下) 经典布局
    bih.biPlanes      = 1;
    bih.biBitCount    = 32;
    bih.biSizeImage   = (DWORD)ents[i].bytes - 40;
    fwrite(&bih, 1, 40, f);
    fwrite(ents[i].xorB.data(), 1, ents[i].xorB.size(), f);
    fwrite(ents[i].andM.data(), 1, ents[i].andM.size(), f);
  }
  fclose(f);
  return true;
}

int main(){
  int sizes[] = {256, 48, 32, 16};
  std::vector<std::pair<int, HBITMAP>> imgs;
  for (int s : sizes) imgs.push_back({s, makeIconBitmap(s)});
  bool ok = writeICO(L"appicon.ico", imgs);
  for (auto& p : imgs) DeleteObject(p.second);
  fwprintf(stderr, ok ? L"已生成 appicon.ico\n" : L"生成失败\n");
  return ok ? 0 : 1;
}
