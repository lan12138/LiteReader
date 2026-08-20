// LiteReader - 轻量级原生代码阅读器 (Win32 / C++)
// 零依赖，静态编译，单 exe。支持语法高亮、彩虹括号、行号、平滑选择、多编码、多标签。
// 作者注：整个程序只有一个 .cpp 文件 + 一个 .cpp 编译出的 .exe，无需任何第三方库。
#define WIN32_LEAN_AND_MEAN          // 只引入最小化的 Win32 头，减少编译体积
#define _WIN32_WINNT 0x0501          // 启用 TrackMouseEvent 等 XP+ API
#include <windows.h>                 // Win32 API 核心（窗口、消息、GDI）
#include <commdlg.h>                 // 通用对话框（打开文件对话框 OPENFILENAME）
#include <shellapi.h>                // 拖拽文件支持（DragAcceptFiles / HDROP）
#include <string>                    // std::wstring 等
#include <vector>                    // 动态数组（文本行、token、文档等）
#include <set>                       // 关键字集合（O(log n) 查找）
#include <algorithm>                 // std::max 等
#include <cwctype>                   // iswupper / iswalnum / iswdigit（宽字符版）
#include <cwchar>                    // 宽字符处理
#include <cstdio>                    // _snwprintf 等
#include <shlobj.h>                   // SHBrowseForFolder / SHGetPathFromIDList（打开文件夹对话框）
#include <objbase.h>                  // CoInitializeEx / CoTaskMemFree

// ----------------------------------------------------------------------------
// 标签栏布局常量
// ----------------------------------------------------------------------------
const int TAB_H   = 26;   // 标签栏高度（像素）
const int TAB_W   = 160;  // 每个标签宽度（像素）
const int TAB_X0  = 0;    // 第一个标签起始 x 坐标
const int PLUS_W  = 24;   // 右上角“新建标签”按钮宽度

// 左侧文件夹浏览器（VSCode 风格）布局常量
const int SIDEBAR_W      = 240; // 侧栏宽度（像素）；未打开文件夹时为 0
const int SIDEBAR_ROW_H  = 22;  // 树每行高度
const int SIDEBAR_HEAD_H = 30;  // 侧栏顶部标题栏高度
const int SIDEBAR_INDENT = 16;  // 每级缩进像素

// 稳定窗口类名（单实例 FindWindow 用，保证每次运行类名一致可被找到）
const wchar_t* WNDCLASS_NAME = L"LiteReaderWndClass_v1";

// ----------------------------------------------------------------------------
// 全局状态
// 说明：整个程序采用“全局视图状态”模型——当前激活标签的文本/光标/滚动等信息
//       都存放在下面这组全局变量里。切换标签时通过 snapshotTo / restoreFrom
//       在“全局变量”与“每个文档的快照 Doc”之间互相拷贝。
// ----------------------------------------------------------------------------
HINSTANCE g_hInst = NULL;             // 当前模块实例句柄
HWND      g_hwnd  = NULL;             // 主窗口句柄
HWND      g_hFind = NULL;             // 查找输入框（NULL 表示未显示查找条）
// 查找条上的“上一项/下一项/关闭”改为“自绘区域”（非子控件），以下为它们的命中矩形与交互状态
const int FIND_H = 30;                // 查找条高度（像素）
RECT     g_rPrev={0}, g_rNext={0}, g_rClose={0}; // 三个按钮的命中矩形
int      g_findHover = 0;             // 当前鼠标悬停的按钮：0=无 1=上一项 2=下一项 3=关闭
int      g_findPress = 0;             // 当前按下的按钮（同上枚举），用于按下态绘制

// 双击分词高亮：g_markWord 为当前标记词，g_markFlag 逐字符标记命中，g_markRanges 为所有命中区间
std::wstring g_markWord;
std::vector<char> g_markFlag;                       // 长度等于 g_text.size()，命中字符为 1
std::vector<std::pair<int,int>> g_markRanges;       // 所有“整词”命中的 [start,end)

std::wstring g_text;                 // 整个文件内容（统一转换为 UTF-16 宽字符串）
std::vector<int> g_lineStart;        // 每一行起始字符在整个 g_text 中的偏移
std::vector<int> g_lineDepth;        // 每一行起始处的括号嵌套深度（用于彩虹括号续行着色）
std::vector<bool> g_lineInBC;        // 每一行起始是否处于块注释 /* */ 中（跨行状态）
std::vector<bool> g_lineInSrv;       // 每一行起始是否处于 ASPX 服务端代码块 <% %> 中
std::vector<int>  g_lineInBlock;     // 每一行起始是否处于 <script>(1)/<style>(2) 子语言块中（混合语言着色）
std::vector<wchar_t> g_lineBsQ;      // 每一行起始处的块字符串引号（python 的三引号 """）
std::vector<int> g_lineLen;          // 每一行长度（字符数，不含换行符）
int g_lineCount = 0;                 // 总行数

// 语法 token 缓存：每行解析出来的着色片段
enum TokType { T_TEXT=0, T_KEYWORD, T_TYPE, T_STRING, T_COMMENT, T_NUMBER,
               T_TAG, T_ATTR, T_AVAL, T_IDENT, T_PUNCT, T_BRACKET,
               T_FUNC, T_PROC, T_PREPROC };
struct Token { int start; int len; unsigned char type; unsigned char col; }; // col 用于括号彩虹色索引
std::vector<std::vector<Token>> g_tokens;   // 每行的 token 列表（懒缓存，见 lineTokens）
std::vector<bool> g_tokDone;               // 对应行是否已经解析过 token

// 视图（全局共享，跨标签保持一致）
bool g_dark = true;   // 默认深色模式
bool g_wrap = false;  // 是否自动换行
int  g_fontSize = 14; // 字号
HFONT g_hFont = NULL; // 当前字体句柄
int  g_charW = 8, g_lineH = 20;   // 单个字符像素宽、单行像素高（measureFont 时测算）
int  g_gutterW = 56;              // 左侧行号区宽度
int  g_topLine = 0;     // 第一条可见“视觉行”的索引（垂直滚动位置）
int  g_scrollX = 0;     // 水平滚动像素
int  g_caretOff = 0;    // 光标字符偏移（相对 g_text 起点）
int  g_anchorOff = 0;   // 选区锚点（按住鼠标拖动时的起点）
int  g_selStart = -1, g_selEnd = -1; // 选中区间 [selStart, selEnd)（-1 表示无选区）
int  g_matchA = -1, g_matchB = -1;    // 括号配对高亮的起始字符偏移（单字符括号或 BEGIN/END 词首）
int  g_matchAw = 1, g_matchBw = 1;    // 配对区间长度（字符括号=1，BEGIN/END=5）

// 视觉行：因“自动换行”，一个逻辑行可能拆成多个视觉行。
// Visual 记录该视觉行属于哪个逻辑行(line)、起始列(col)、长度(len)。
struct Visual { int line; int col; int len; };
std::vector<Visual> g_visual;
int g_visualCount = 0;

std::wstring g_filePath;             // 当前文档的磁盘路径
std::wstring g_lang = L"auto";       // 语言选择（auto / txt / csharp / ...）
int  g_enc = 0;                      // 当前文档编码：0=UTF-8无BOM 1=UTF-8 BOM 2=UTF-16LE 3=UTF-16BE 4=ANSI(GBK)
bool g_dirty = false;                // 文档是否已修改（未保存），标题追加 “ *” 提示

// 语言枚举
enum Lang { L_AUTO, L_TXT, L_CS, L_SQL, L_HTML, L_JS, L_JSON, L_PY, L_CSS, L_XML,
            L_C, L_CPP, L_JAVA, L_ASPX };
Lang g_langId = L_AUTO;              // 实际生效的语言 id

// 关键字集合（不同语言分开存）
std::set<std::wstring> KW_CS, KW_SQL, KW_JS, KW_PY, KW_CSS, TY_CS,
                      KW_C, KW_CPP, KW_JAVA, TY_C, TY_CPP, TY_JAVA;
// 前缀 KW_ 为关键字，TY_ 为类型/内置类型名（着色为 T_TYPE）

// ----------------------------------------------------------------------------
// 多文档（标签）
// ----------------------------------------------------------------------------
struct Doc {
  std::wstring text;                          // 该文档的文本内容
  std::vector<int> lineStart, lineLen, lineDepth;
  std::vector<bool> lineInBC;                 // 注意：结构体里用的是 lineInBC，全局里也叫 lineInBC
  std::vector<bool> lineInSrv;
  std::vector<wchar_t> lineBsQ;
  std::vector<std::vector<Token>> tokens;
  std::vector<bool> tokDone;
  int lineCount = 0;
  std::wstring filePath, lang;
  Lang langId = L_AUTO;
  int caretOff=0, anchorOff=0, selStart=-1, selEnd=-1, matchA=-1, matchB=-1, matchAw=1, matchBw=1;
  int topLine=0, scrollX=0;
  std::vector<Visual> visual;
  int visualCount = 0;
  bool dirty = false;                         // 该文档是否已修改（未保存）
  std::wstring title;                         // 预留标题字段（当前用 filePath 派生，未单独使用）
};
std::vector<Doc> g_docs;          // 所有打开的文档
int g_active = -1;                // 当前激活标签索引

// 把当前“全局视图状态”快照保存到文档 i（切换标签前先调用，避免丢失当前页状态）
void snapshotTo(int i){
  if(i<0||i>=(int)g_docs.size()) return;
  Doc& d=g_docs[i];
  d.text=g_text; d.lineStart=g_lineStart; d.lineLen=g_lineLen; d.lineDepth=g_lineDepth;
  d.lineInBC=g_lineInBC; d.lineInSrv=g_lineInSrv; d.lineBsQ=g_lineBsQ; d.tokens=g_tokens; d.tokDone=g_tokDone;
  d.lineCount=g_lineCount; d.filePath=g_filePath; d.lang=g_lang; d.langId=g_langId;
  d.caretOff=g_caretOff; d.anchorOff=g_anchorOff; d.selStart=g_selStart; d.selEnd=g_selEnd;
  d.matchA=g_matchA; d.matchB=g_matchB; d.matchAw=g_matchAw; d.matchBw=g_matchBw; d.topLine=g_topLine; d.scrollX=g_scrollX;
  d.dirty=g_dirty;
  d.visual=g_visual; d.visualCount=g_visualCount;
}
// 从文档 i 恢复“全局视图状态”（切换标签进来时调用）
void restoreFrom(int i){
  if(i<0||i>=(int)g_docs.size()) return;
  const Doc& d=g_docs[i];
  g_text=d.text; g_lineStart=d.lineStart; g_lineLen=d.lineLen; g_lineDepth=d.lineDepth;
  g_lineInBC=d.lineInBC; g_lineInSrv=d.lineInSrv; g_lineBsQ=d.lineBsQ; g_tokens=d.tokens; g_tokDone=d.tokDone;
  g_lineCount=d.lineCount; g_filePath=d.filePath; g_lang=d.lang; g_langId=d.langId;
  g_caretOff=d.caretOff; g_anchorOff=d.anchorOff; g_selStart=d.selStart; g_selEnd=d.selEnd;
  g_matchA=d.matchA; g_matchB=d.matchB; g_matchAw=d.matchAw; g_matchBw=d.matchBw; g_topLine=d.topLine; g_scrollX=d.scrollX;
  g_dirty=d.dirty;
  g_visual=d.visual; g_visualCount=d.visualCount;
}

// ----------------------------------------------------------------------------
// 颜色
// ----------------------------------------------------------------------------
// One Dark Pro 风格调色板。注意 COLORREF 内存布局为 0x00BBGGRR（B 在低位，R 在高位）。
// 例如 0x00DD78C6：RR=DD GG=78 BB=C6，对应 #c678dd。
COLORREF C_LIGHT[T_PREPROC+1] = {
  0x002E2924, // TEXT
  0x00A426A6, // KEYWORD (purple)
  0x00016898, // TYPE (yellow-brown)
  0x004AA150, // STRING (green)
  0x00998F8B, // COMMENT (gray)
  0x00164BCB, // NUMBER (orange)
  0x005B18C2, // TAG (red)
  0x00016898, // ATTR (yellow-brown)
  0x004AA150, // AVAL (green)
  0x002E2924, // IDENT
  0x006A6057, // PUNCT
  0x000000,   // BRACKET (overridden by rainbow)
  0x00F27840, // FUNC (blue)
  0x00164BCB, // PROC (orange)
  0x00BC8401  // PREPROC (cyan-blue)
};
COLORREF C_DARK[T_PREPROC+1] = {
  0x00BFB2AB, // TEXT     (#abb2bf)
  0x00DD78C6, // KEYWORD  (#c678dd)
  0x007BC0E5, // TYPE     (#e5c07b)
  0x0079C398, // STRING   (#98c379)
  0x0070635C, // COMMENT  (#5c6370)
  0x00669AD1, // NUMBER   (#d19a66)
  0x00756CE0, // TAG      (#e06c75)
  0x00669AD1, // ATTR     (#d19a66)
  0x0079C398, // AVAL     (#98c379)
  0x00BFB2AB, // IDENT    (#abb2bf)
  0x00968A82, // PUNCT    (#828a96)
  0x000000,   // BRACKET  (overridden by rainbow)
  0x00EFAF61, // FUNC     (#61afef)
  0x00669AD1, // PROC     (#d19a66)
  0x00C2B656  // PREPROC  (#56b6c2)
};
// 彩虹括号配色：按嵌套深度 depth%6 取色，最多 6 种循环
COLORREF RB_LIGHT[6] = {0x003439C2,0x000089B5,0x003B8A2F,0x00A3851A,0x00BF6F1F,0x00A34192};
COLORREF RB_DARK[6]  = {0x00756CE0,0x007BC0E5,0x0079C398,0x00C2B656,0x00EFAF61,0x00DD78C6};

// 以下为各界面元素的背景/前景色（依据深浅主题返回）
COLORREF bgColor(){ return g_dark?0x00342C28:0x00FAFAFA; }   // 主背景 #282c34 / #fafafa
COLORREF gutterBg(){ return g_dark?0x002B2521:0x00F0F0F0; }  // 行号区背景 #21252b / #f0f0f0
COLORREF gutterFg(){ return g_dark?0x0070635C:0x00999999; }  // 行号文字 #5c6370 / #999999
COLORREF selBg(){ return g_dark?0x0051443E:0x00FFE8CF; }     // 选区背景 #3E4451 / #cfe8ff
COLORREF matchBg(){ return g_dark?0x00525220:0x00A0F3FF; }    // 括号配对背景 #525220 / #fff3a0
COLORREF markBg(){ return g_dark?0x0046401A:0x00BFE6C0; }    // 分词高亮背景（暗：橄榄绿 / 亮：浅绿）

// ----------------------------------------------------------------------------
// 工具函数
// ----------------------------------------------------------------------------
inline int min3(int a,int b,int c){ int m=a; if(b<m)m=b; if(c<m)m=c; return m; } // 三数取小（预留，当前未大量使用）

// 前向声明：在 scanLine / paint 等函数定义之前，先把会调用到的函数声明出来
void updateScroll();
int leftBar();                     // 左侧栏宽度（未打开文件夹时为 0）
void updateCaretPos();
void setWindowTitle();
std::wstring openFileDialog();
void openInNewTab(const std::wstring& path);
void switchTab(int j);
void closeTab(int i);

// 二分查找：给定字符偏移 off，返回它属于第几行
// g_lineStart 是单调递增的，所以用二分；结果 res 为最后一个 <= off 的行。
int lineOfOffset(int off){
  int lo=0, hi=g_lineCount-1, res=0;
  while(lo<=hi){
    int mid=(lo+hi)/2;
    if(g_lineStart[mid]<=off){ res=mid; lo=mid+1; } else hi=mid-1;
  }
  return res;
}
// 给定行与偏移，返回该偏移在行内的列号（偏移 - 行首偏移）
int colOfOffset(int line, int off){ return off - g_lineStart[line]; }

// ---- 字符宽度（支持中文/全角：在等宽字体下占 2 格） ----
// 说明：程序用 Consolas 等宽字体，但等宽字体下 ASCII 占 1 格、CJK/全角占 2 格，
//       因此需要单独判断“宽字符”，才能正确计算像素位置。
inline bool isWideChar(wchar_t c){
  if(c==0) return false;
  if(c>=0x1100 && c<=0x115F) return true;   // 谚文兼容字母
  if(c>=0x2E80 && c<=0x303E) return true;   // CJK 部首/符号/标点
  if(c>=0x3041 && c<=0x33FF) return true;   // 平假名/片假名/CJK 符号
  if(c>=0x3400 && c<=0x4DBF) return true;   // CJK 扩展A
  if(c>=0x4E00 && c<=0x9FFF) return true;   // CJK 统一表意文字（常用汉字）
  if(c>=0xA000 && c<=0xA4CF) return true;   // 彝文
  if(c>=0xAC00 && c<=0xD7A3) return true;   // 谚文音节
  if(c>=0xF900 && c<=0xFAFF) return true;   // 兼容表意
  if(c>=0xFE30 && c<=0xFE4F) return true;   // CJK 兼容形式
  if(c>=0xFF00 && c<=0xFFEF) return true;   // 全角 ASCII / 半角片假名
  if(c>=0x20000 && c<=0x2FA1F) return true; // CJK 扩展B+
  return false;
}
// 返回单个字符的像素宽度（宽字符为字符宽的 2 倍）
inline int charW(wchar_t c){ return isWideChar(c)? g_charW*2 : g_charW; }
// PascalCase / 首字母大写的多字符词 —— 在 C#/JS 中通常代表类型/类名/构造函数
inline bool isUpperWord(const std::wstring& w){
  if(w.size()<2) return false;
  return iswupper((wchar_t)w[0])!=0;
}
// 计算某一行 [0,col) 区间的累计像素宽度（用于把列号换算成 x 坐标）
int linePrefixPx(int line, int col){
  if(col<0)col=0; int s=g_lineStart[line], n=s+col, w=0;
  for(int i=s;i<n;i++) w+=charW(g_text[i]);
  return w;
}
// 整行像素宽度（遍历该行所有字符累加）
int linePx(int line){
  int s=g_lineStart[line], n=s+g_lineLen[line], w=0;
  for(int i=s;i<n;i++) w+=charW(g_text[i]);
  return w;
}
// 字符段 [start,start+len) 的像素宽度
int runPx(int start, int len){
  int w=0; for(int i=start;i<start+len;i++) w+=charW(g_text[i]);
  return w;
}
// 从行首按像素偏移反查列号（取字符“中点”为边界，使鼠标点击命中更自然）
int pxToColAbs(int line, int px){
  int len=g_lineLen[line], w=0;
  for(int c=0;c<len;c++){
    int cw=charW(g_text[g_lineStart[line]+c]);
    if(w + cw/2 >= px) return c;  // 过了字符中点就算到该字符
    w+=cw;
  }
  return len;
}

// 激活已存在窗口：仅当最小化时恢复，否则只置前 —— 不改变大小/位置/最大化状态
void bringToFront(HWND hw){
  if(IsIconic(hw)) ShowWindow(hw,SW_RESTORE); // 最小化则恢复窗口
  SetForegroundWindow(hw);                    // 否则仅提到最前
}

// ----------------------------------------------------------------------------
// 查找条 / 分词高亮：辅助函数
// ----------------------------------------------------------------------------
// 编辑器可视区顶部 y：显示查找条时整体下移 FIND_H，避免首行被查找条遮挡。
int editorTop(){ return TAB_H + (g_hFind?FIND_H:0); }

// 判断字符是否构成“单词”（标识符）的一部分：字母/数字/下划线/美元符/Python 的 @
inline bool isWordChar(wchar_t c){
  return (iswalnum((wchar_t)c)!=0) || c==L'_' || c==L'$' || c==L'@';
}
// 给定光标偏移 off，向左右扩展出完整单词边界，写入 [ws,we)
void wordAtOffset(int off, int& ws, int& we){
  if(off<0||off>=(int)g_text.size()){ ws=off; we=off; return; }
  ws=off; we=off+1;
  while(ws>0 && isWordChar(g_text[ws-1])) ws--;          // 向左扩展
  while(we<(int)g_text.size() && isWordChar(g_text[we])) we++; // 向右扩展
}
// 收集 g_markWord 的所有“整词”命中区间（前后均非单词字符，避免 in 命中 index 这类子串），
// 结果写入 g_markRanges 与逐字符标记 g_markFlag（供 paint 直接查表）。
void collectMarks(){
  g_markRanges.clear();
  int n=(int)g_markWord.size();
  g_markFlag.assign((int)g_text.size(),0); // 重置为全 0
  if(n==0) return;
  int len=(int)g_text.size();
  for(int p=0; p+n<=len; ){
    if(g_text.compare(p,n,g_markWord)==0){
      bool okPrev = (p==0)        || !isWordChar(g_text[p-1]);   // 词首前不能是单词字符
      bool okNext = (p+n>=len)    || !isWordChar(g_text[p+n]);   // 词尾后不能是单词字符
      if(okPrev && okNext){
        g_markRanges.push_back({p,p+n});
        for(int k=p;k<p+n;k++) g_markFlag[k]=1;
      }
      p+=n; // 跳过本词，避免同位置重复
    } else p++;
  }
}
// 自绘一个圆角按钮（用于查找条的“上一项/下一项”）
void drawFindButton(HDC hdc, RECT r, const wchar_t* text, bool hover, bool pressed, bool dark){
  COLORREF base = dark?0x3A6EA5:0x2F7FD1;                 // 默认蓝
  COLORREF col  = pressed ? (dark?0x2A5278:0x1F5FA0)
                : (hover  ? (dark?0x4A82BE:0x4A95DD) : base);
  HBRUSH b=CreateSolidBrush(col);
  HPEN   p=CreatePen(PS_SOLID,1,col);                    // 边框同色，避免深色描边
  HPEN   op=(HPEN)SelectObject(hdc,p);
  HBRUSH ob=(HBRUSH)SelectObject(hdc,b);
  RoundRect(hdc,r.left,r.top,r.right,r.bottom,6,6);      // 圆角矩形：填充+描边
  SelectObject(hdc,op); DeleteObject(p);
  SelectObject(hdc,ob); DeleteObject(b);
  SetBkMode(hdc,TRANSPARENT);
  SetTextColor(hdc,RGB(255,255,255));
  DrawText(hdc,text,-1,&r,DT_CENTER|DT_SINGLELINE|DT_VCENTER|DT_NOPREFIX);
}
// 自绘关闭按钮（×）：普通为中性灰，悬停时变红
void drawFindCloseBtn(HDC hdc, RECT r, bool hover, bool pressed, bool dark){
  COLORREF col = hover ? (dark?0x8A3B3B:0xC0392B) : (dark?0x3A3A3A:0xD0D0D0);
  HBRUSH b=CreateSolidBrush(col);
  HPEN   p=CreatePen(PS_SOLID,1,col);
  HPEN   op=(HPEN)SelectObject(hdc,p);
  HBRUSH ob=(HBRUSH)SelectObject(hdc,b);
  RoundRect(hdc,r.left,r.top,r.right,r.bottom,6,6);
  SelectObject(hdc,op); DeleteObject(p);
  SelectObject(hdc,ob); DeleteObject(b);
  SetBkMode(hdc,TRANSPARENT);
  SetTextColor(hdc, hover ? RGB(255,255,255) : (dark?RGB(200,200,200):RGB(60,60,60)));
  DrawText(hdc,L"×",1,&r,DT_CENTER|DT_SINGLELINE|DT_VCENTER|DT_NOPREFIX);
}

// ----------------------------------------------------------------------------
// 语言识别
// 根据 g_lang（用户选择）与 g_filePath（扩展名）推导实际语言枚举。
// g_lang 为 "auto" 时按扩展名推断；否则按用户指定字符串直接映射。
// ----------------------------------------------------------------------------
Lang langFromName(){
  if(g_lang==L"auto"){
    if(g_filePath.empty()) return L_TXT;
    std::wstring ext = g_filePath.substr(g_filePath.find_last_of(L'.')+1); // 取扩展名
    if(ext==L"cs") return L_CS;
    if(ext==L"sql") return L_SQL;
    if(ext==L"html"||ext==L"htm") return L_HTML;
    if(ext==L"js"||ext==L"mjs"||ext==L"cjs") return L_JS;
    if(ext==L"json") return L_JSON;
    if(ext==L"py"||ext==L"pyw") return L_PY;
    if(ext==L"css") return L_CSS;
    if(ext==L"c"||ext==L"h") return L_C;
    if(ext==L"cpp"||ext==L"cc"||ext==L"cxx"||ext==L"c++"||ext==L"hpp"||ext==L"hxx"||ext==L"hh") return L_CPP;
    if(ext==L"java") return L_JAVA;
    if(ext==L"aspx"||ext==L"asax"||ext==L"ascx"||ext==L"ashx"||ext==L"asmx"||ext==L"master") return L_ASPX;
    if(ext==L"xml"||ext==L"xaml"||ext==L"svg"||ext==L"config"||ext==L"csproj"||ext==L"vcxproj"||ext==L"resx") return L_XML;
    return L_TXT;
  }
  // 用户手动指定了语言
  if(g_lang==L"txt") return L_TXT;
  if(g_lang==L"csharp") return L_CS;
  if(g_lang==L"sql") return L_SQL;
  if(g_lang==L"html") return L_HTML;
  if(g_lang==L"js") return L_JS;
  if(g_lang==L"json") return L_JSON;
  if(g_lang==L"python") return L_PY;
  if(g_lang==L"css") return L_CSS;
  if(g_lang==L"c") return L_C;
  if(g_lang==L"cpp") return L_CPP;
  if(g_lang==L"java") return L_JAVA;
  if(g_lang==L"aspx") return L_ASPX;
  if(g_lang==L"xml") return L_XML;
  return L_TXT;
}

// ----------------------------------------------------------------------------
// 分词（同时用于跨行状态扫描）
// 这是整个着色器的核心：一行一行地扫描文本，产出 token（着色片段），
// 同时维护跨行状态（是否在块注释中、括号深度、是否在 ASPX 服务端块中、块字符串引号）。
// 参数 out==nullptr 时只更新状态（用于 rebuildLines 计算每行起始状态，不生成 token）；
//      out!=nullptr 时既更新状态又生成 token 列表（用于 lineTokens 实际着色）。
// s/i/n : 待扫描字符串、其长度、语言。
// inBC/inBlock/inSrv/bsQ/depth 均为“输入输出”引用：进入本行前的状态；函数结束时写回行末状态。
// ----------------------------------------------------------------------------
void scanLine(const wchar_t* s, int n, Lang lang, bool& inBC, int& inBlock, bool& inSrv, wchar_t& bsQ, int& depth,
              std::vector<Token>* out){
  int i=0;
  bool isClike = (lang==L_CS||lang==L_JS||lang==L_JSON||lang==L_CSS||lang==L_SQL||
                  lang==L_C||lang==L_CPP||lang==L_JAVA);
  bool lineComment = false; wchar_t lcChar=0;
  if(lang==L_CS||lang==L_JS||lang==L_CSS||lang==L_C||lang==L_CPP||lang==L_JAVA) lcChar=L'/'; // C 族行注释 //
  else if(lang==L_SQL) lcChar=L'-';   // SQL 行注释 --
  else if(lang==L_PY) lcChar=L'#';    // Python 行注释 #
  lineComment = (lcChar != 0);        // 行注释开关
  bool blockCmt = (lang==L_CS||lang==L_JS||lang==L_CSS||lang==L_SQL||
                   lang==L_C||lang==L_CPP||lang==L_JAVA); // 是否支持 /* */ 块注释
  bool afterProcKw = false;   // 上一个关键字是 EXEC/PROCEDURE 时，下一个标识符即存储过程名
  bool afterProcDot = false;  // 过程名被 schema 限定(dbo.x)时，跳过 schema 名，染真正的 proc 名
  // 局部 lambda：若有 out 则把 token 追加进去
  auto add=[&](int st,int len,TokType t,unsigned char col){
    if(out) out->push_back({st,len,(unsigned char)t,col});
  };
  while(i<n){
    wchar_t c=s[i];
    // ---- 行首若已经在块注释中：一直吃到 */ 结束 ----
    if(inBC){
      int j=i; while(j+1<n && !(s[j]==L'*'&&s[j+1]==L'/')) j++;
      if(j+1<n){ add(i,j+2-i,T_COMMENT,0); inBC=false; i=j+2; } // 找到 */，整段作为注释，离开块注释
      else { add(i,n-i,T_COMMENT,0); i=n; }                    // 到行尾都没找到 */，整行是注释
      continue;
    }
    // ---- C/C++/Java 预处理器指令（#include / #define / #pragma ...） ----
    // 条件：行首（或仅空白后）以 # 开头。
    if((lang==L_C||lang==L_CPP||lang==L_JAVA) && c==L'#'){
      int k2=0; while(k2<i && (s[k2]==L' '||s[k2]==L'\t')) k2++; // 数前导空白
      if(k2==i){ // # 之前只有空白 => 视为预处理指令
        int j=i+1; while(j<n && (iswalnum(s[j])||s[j]==L'_')) j++; // 读指令名
        add(i, j-i, T_PREPROC, 0);
        i=j; continue;
      }
    }
    // ---- HTML / ASPX / XML 标签与结构 ----
    if(lang==L_HTML||lang==L_ASPX||lang==L_XML){
      // 混合语言：已在 <script>(JS) 或 <style>(CSS) 子块内，整段按对应子语言着色，
      // 直到遇到对应的结束标签（跨行状态由 inBlock 维护：0=无 1=script/JS 2=style/CSS）。
      if(inBlock!=0){
        Lang sub = (inBlock==1)? L_JS : L_CSS;                 // 子块对应的子语言
        const wchar_t* closeTok = (inBlock==1)? L"</script" : L"</style"; // 对应的结束标签前缀
        int cLen=(int)wcslen(closeTok);
        int j=i; bool foundClose=false;
        while(j+cLen-1 < n){ if(wcsncmp(s+j,closeTok,cLen)==0){ foundClose=true; break; } j++; }
        int fragLen = foundClose? (j-i) : (n-i);               // 本行内子语言片段长度
        std::vector<Token> tmp;
        bool x=inBC; int ib=0; bool is=false; wchar_t z=bsQ; int d=depth;
        scanLine(s+i, fragLen, sub, x, ib, is, z, d, &tmp);    // 递归用 JS/CSS 规则扫描（子块内不嵌套）
        for(auto& t: tmp){ t.start += i; add(t.start, t.len, (TokType)t.type, t.col); }
        inBC=x; depth=d; bsQ=z;                               // 回写跨行状态
        if(foundClose){
          int k=j+cLen; while(k<n && s[k]!=L'>') k++;          // 跳过结束标签到 '>'
          int e=(k<n)? k+1 : n;
          add(j, e-j, T_TAG, 0);                              // 结束标签整体按标签色
          inBlock=0; i=e; continue;                           // 退出子块
        } else { i=n; continue; }                             // 子块跨行，状态保留
      }
      // ASPX 服务端代码块：<% ... %> 内的内容按 C# 着色（且可跨行，状态由 inSrv 维护）。仅 HTML/ASPX 生效，XML 不处理。
      if((lang==L_HTML||lang==L_ASPX) && inSrv){
        int j=i; while(j+1<n && !(s[j]==L'%'&&s[j+1]==L'>')) j++; // 找 %>
        int fragLen=(j+1<n)? j-i : n-i;  // 本行内服务端片段长度
        std::vector<Token> tmp;
        bool x=inBC; int ib2=0; bool srv=false; wchar_t z=0; int d=depth;
        scanLine(s+i, fragLen, L_CS, x, ib2, srv, z, d, &tmp); // 递归用 C# 规则扫描这段
        for(auto& t: tmp){ t.start += i; add(t.start, t.len, (TokType)t.type, t.col); }
        depth=d;
        if(j+1<n){ add(j,2,T_PUNCT,0); inSrv=false; inBC=false; i=j+2; } // 遇到 %>，闭合服务端块
        else { i=n; }                                                  // 服务端块跨行，状态保留
        continue;
      }
      // 进入服务端块：<% 或 <%@ / <%= / <%# / <%$ / <% （仅 HTML/ASPX，XML 的 < 一律按标签处理）
      if((lang==L_HTML||lang==L_ASPX) && c==L'<' && i+1<n && s[i+1]==L'%'){
        wchar_t d2=(i+2<n)? s[i+2]:0;
        int plen=2; if(d2=='@'||d2=='='||d2=='#'||d2=='$'||d2==':') plen=3; // 带修饰符的块多一个字符
        add(i,plen,T_PUNCT,0); inSrv=true; i+=plen; continue;
      }
      // XML 处理指令 <?xml ... ?> 与 <? ... ?>（声明 / PI），用注释色
      if(lang==L_XML && c==L'<' && i+1<n && s[i+1]==L'?'){
        int j=i+2; while(j+1<n && !(s[j]==L'?'&&s[j+1]==L'>')) j++;
        int end=(j+1<n)? j+2 : n; add(i,end-i,T_COMMENT,0); i=end; continue;
      }
      // XML CDATA 段 <![CDATA[ ... ]]>，整体作为字符串色突出
      if(lang==L_XML && c==L'<' && i+3<n && s[i+1]==L'!' && s[i+2]==L'[' && s[i+3]==L'C'){
        int j=i+4; while(j+2<n && !(s[j]==L']'&&s[j+1]==L']'&&s[j+2]==L'>')) j++;
        int end=(j+2<n)? j+3 : n; add(i,end-i,T_STRING,0); i=end; continue;
      }
      // XML 的 <!DOCTYPE ...> / <!ENTITY ...> 等声明块（排除 <!-- 注释，下面单独处理）
      if(lang==L_XML && c==L'<' && i+1<n && s[i+1]==L'!' && !(i+3<n && s[i+2]==L'-'&&s[i+3]==L'-')){
        int j=i+2; while(j<n && s[j]!=L'>') j++;
        int end=(j<n)? j+1 : n; add(i,end-i,T_COMMENT,0); i=end; continue;
      }
      // XML 实体引用 &name; / &#123;，着色为字符串色
      if(lang==L_XML && c==L'&'){
        int j=i+1; while(j<n && (iswalnum(s[j])||s[j]==L'#')) j++;
        if(j<n && s[j]==L';'){ add(i,j+1-i,T_STRING,0); i=j+1; continue; }
      }
      // HTML 条件注释 <!-- -->
      if(c==L'<' && i+3<n && s[i+1]==L'!' && s[i+2]==L'-' && s[i+3]==L'-'){
        int j=i+4; while(j+2<n && !(s[j]==L'-'&&s[j+1]==L'-'&&s[j+2]==L'>')) j++;
        int end=(j+2<n)?j+3:n; add(i,end-i,T_COMMENT,0); i=end; continue;
      }
      // 普通标签：<tag attr="val"> 结构着色
      if(c==L'<'){
        int j=i+1;
        add(i,1,T_PUNCT,0); // '<'
        if(j<n && s[j]==L'/'){ add(j,1,T_PUNCT,0); j++; } // 闭合标签的 '/'
        int ns=j; while(j<n && (iswalnum(s[j])||s[j]==L':'||s[j]==L'-')) j++;
        std::wstring tagName;
        if(j>ns){ tagName=std::wstring(s+ns,j-ns); add(ns,j-ns,T_TAG,0); } // 标签名（同时记录供子语言判断）
        while(j<n && s[j]!=L'>'){
          wchar_t ch=s[j];
          if(ch==L' '||ch==L'\t'||ch==L'\n'||ch==L'\r'){ add(j,1,T_TEXT,0); j++; continue; }
          if(ch==L'='){ add(j,1,T_PUNCT,0); j++; continue; }
          if(ch==L'"'||ch==L'\''){ // 属性值字符串
            wchar_t q=ch; int k=j+1; while(k<n && s[k]!=q) k++;
            int e=(k<n)?k+1:k; add(j,e-j,T_AVAL,0); j=e; continue;
          }
          int as=j; while(j<n && (iswalnum(s[j])||s[j]==L':'||s[j]==L'-'||s[j]==L'.')) j++;
          if(j>as) add(as,j-as,T_ATTR,0); // 属性名
          else { add(j,1,T_TEXT,0); j++; }
        }
        if(j<n && s[j]==L'>'){ add(j,1,T_PUNCT,0); j++; } // '>'
        // 进入 <script>/<style> 开始标签后，后续内容按对应子语言着色
        if(i+1<n && s[i+1]!=L'/'){ // 仅对开始标签（非 </x>）生效
          bool isScript=(tagName.size()==6); bool isStyle=(tagName.size()==5);
          const wchar_t* s6=L"script", *s5=L"style";
          if(isScript){ for(int q=0;q<6;q++) if(towlower(tagName[q])!=s6[q]){isScript=false;break;} }
          if(isStyle){ for(int q=0;q<5;q++) if(towlower(tagName[q])!=s5[q]){isStyle=false;break;} }
          if(isScript) inBlock=1; else if(isStyle) inBlock=2;
        }
        i=j; continue;
      }
      // HTML 中的括号也做彩虹着色
      if(c==L'('||c==L'['||c==L'{'){ int col=(unsigned char)(depth%6); add(i,1,T_BRACKET,col); depth++; i++; continue; }
      if(c==L')'||c==L']'||c==L'}'){ int d=depth>0?depth-1:0; int col=(unsigned char)(d%6); add(i,1,T_BRACKET,col); if(depth>0)depth--; i++; continue; }
      add(i,1,T_TEXT,0); i++; continue;
    }
    // ---- 行注释（// 或 -- 或 #）直到行尾 ----
    if(lineComment && lcChar==L'/' && i+1<n && s[i]==L'/' && s[i+1]==L'/'){
      add(i,n-i,T_COMMENT,0); i=n; continue;
    }
    if(lineComment && lcChar==L'-' && i+1<n && s[i]==L'-' && s[i+1]==L'-'){
      add(i,n-i,T_COMMENT,0); i=n; continue;
    }
    if(lineComment && lcChar==L'#' && c==L'#'){
      add(i,n-i,T_COMMENT,0); i=n; continue;
    }
    // ---- 块注释 /* */ ----
    if(blockCmt && i+1<n && s[i]==L'/' && s[i+1]==L'*'){
      int j=i+2; while(j+1<n && !(s[j]==L'*'&&s[j+1]==L'/')) j++;
      int end=(j+1<n)?j+2:n; add(i,end-i,T_COMMENT,0);
      if(j+1<n) inBC=false; else inBC=true; // 本行内闭合则离开；否则标记跨行
      i=end; continue;
    }
    // ---- Python 三引号块字符串 """ / ''' ----
    if(lang==L_PY && i+2<n && s[i]==L'"' && s[i+1]==L'"' && s[i+2]==L'"'){
      int j=i+3; while(j+2<n && !(s[j]==L'"'&&s[j+1]==L'"'&&s[j+2]==L'"')) j++;
      int end=(j+2<n)?j+3:n; add(i,end-i,T_STRING,0); i=end; continue;
    }
    // ---- 普通字符串字面量（按语言区分定界符） ----
    bool isStr=false; wchar_t q=0;
    if(lang==L_CS||lang==L_JS||lang==L_C||lang==L_CPP||lang==L_JAVA){
      if(c==L'"'||c==L'\''||(lang==L_JS&&c==L'`')){ q=c; isStr=true; } // JS 额外支持模板字符串 `
    } else if(lang==L_SQL){
      if(c==L'\''){ q=c; isStr=true; }            // SQL 仅单引号
    } else if(lang==L_PY){
      if(c==L'"'||c==L'\''){ q=c; isStr=true; }
    } else if(lang==L_JSON){
      if(c==L'"'){ q=c; isStr=true; }             // JSON 仅双引号
    } else if(lang==L_CSS){
      if(c==L'"'||c==L'\''){ q=c; isStr=true; }
    }
    if(isStr){
      int j=i+1;
      if(lang==L_SQL){
        // SQL 中两个单引号 '' 表示转义的单引号，需跳过
        while(j<n){ if(s[j]==L'\''){ if(j+1<n && s[j+1]==L'\''){ j+=2; continue; } else { j++; break; } } j++; }
      } else {
        // 其它语言：处理转义符 \x，遇引号结束，遇换行（非 JS 模板串）停止
        while(j<n){ if(s[j]==L'\\' && j+1<n){ j+=2; continue; } if(s[j]==q){ j++; break; } if(s[j]==L'\n') break; j++; }
      }
      add(i,j-i,T_STRING,0); i=j; continue;
    }
    // ---- 数字字面量（含十六进制、科学计数、类型后缀） ----
    if(isClike || lang==L_PY){
      if(iswdigit(c) || (c==L'.' && i+1<n && iswdigit(s[i+1]))){
        int j=i;
        while(j<n){
          wchar_t ch=s[j];
          if(iswdigit(ch)||ch==L'.'||ch==L'_'||ch==L'\''||ch==L'x'||ch==L'X') { j++; continue; }
          if((ch>=L'a'&&ch<=L'f')||(ch>=L'A'&&ch<=L'F')||ch==L'u'||ch==L'U'||ch==L'l'||ch==L'L'||ch==L'p'||ch==L'P') { j++; continue; }
          break;
        }
        add(i,j-i,T_NUMBER,0); i=j; continue;
      }
    }
    // ---- 标识符（含关键字/类型/函数名判定） ----
    if(iswalpha(c)||c==L'_'||c==L'$'||(lang==L_PY&&c==L'@')){
      int j=i; while(j<n && (iswalnum(s[j])||s[j]==L'_'||s[j]==L'$'||(lang==L_PY&&s[j]==L'@'))) j++;
      std::wstring w(s+i,j-i);
      // SQL：BEGIN / CASE 当作块左括号，END 当作块右括号，按 depth 做彩虹着色并维护深度，
      // 这样它们能与 ()[]{} 一起形成正确嵌套的配对高亮（如 CASE...END、BEGIN...END），而非普通关键字。
      if(lang==L_SQL && (w==L"BEGIN"||w==L"END"||w==L"CASE")){
        if(w==L"END"){ int d=depth>0?depth-1:0; int col=(unsigned char)(d%6); add(i,j-i,T_BRACKET,col); if(depth>0)depth--; } // 右括号（END 关闭 BEGIN 或 CASE）
        else          { int col=(unsigned char)(depth%6); add(i,j-i,T_BRACKET,col); depth++; }   // 左括号 BEGIN / CASE
        i=j; continue;
      }
      TokType t=T_IDENT;
      // 依据语言查关键字表/类型表，并把 PascalCase 词判定为类型
      if(lang==L_CS){
        if(KW_CS.count(w)) t=T_KEYWORD; else if(TY_CS.count(w)) t=T_TYPE;
        else if(isUpperWord(w)) t=T_TYPE;            // PascalCase => 类型/类名/变量类型
      } else if(lang==L_SQL){
        if(KW_SQL.count(w)) t=T_KEYWORD;
      } else if(lang==L_JS){
        if(KW_JS.count(w)) t=T_KEYWORD;
        else if(isUpperWord(w)) t=T_TYPE;            // 构造函数/类名
      } else if(lang==L_PY){
        if(KW_PY.count(w)) t=T_KEYWORD;
      } else if(lang==L_CSS){
        if(KW_CSS.count(w)) t=T_KEYWORD;
      } else if(lang==L_JSON){
        if(w==L"true"||w==L"false"||w==L"null") t=T_KEYWORD;
      } else if(lang==L_C){
        if(KW_C.count(w)) t=T_KEYWORD; else if(TY_C.count(w)) t=T_TYPE;
        else if(isUpperWord(w)) t=T_TYPE;            // PascalCase => 类型/类名
      } else if(lang==L_CPP){
        if(KW_CPP.count(w)) t=T_KEYWORD; else if(TY_CPP.count(w)) t=T_TYPE;
        else if(isUpperWord(w)) t=T_TYPE;            // PascalCase => 类型/类名
      } else if(lang==L_JAVA){
        if(KW_JAVA.count(w)) t=T_KEYWORD; else if(TY_JAVA.count(w)) t=T_TYPE;
        else if(isUpperWord(w)) t=T_TYPE;            // PascalCase => 类型/类名
      }
      // EXEC / PROCEDURE 后的标识符 => 存储过程名（支持 dbo.procName 形式）
      int d2=j; while(d2<n && (s[d2]==L' '||s[d2]==L'\t')) d2++;
      bool followedByDot = (d2<n && s[d2]==L'.');
      if(lang==L_SQL && afterProcKw && t==T_IDENT){
        if(followedByDot){ afterProcKw=false; afterProcDot=true; } // schema 限定名，跳过 schema 部分
        else { t=T_PROC; afterProcKw=false; }
      }
      // 函数/存储过程调用：标识符后紧跟 '('（忽略空白）
      int k=j; while(k<n && (s[k]==L' '||s[k]==L'\t')) k++;
      if(k<n && s[k]==L'('){
        if(lang==L_SQL) t=T_PROC;                    // SQL 中任何后接 '(' 的标识符视为函数/过程
        else if(t==T_KEYWORD) { /* C#/JS 控制流关键字(if/for/while...)保持关键字色 */ }
        else if(t==T_TYPE)   { /* 构造函数(new List())保持类型色 */ }
        else t=T_FUNC;
      }
      add(i,j-i,t,0);
      if(lang==L_SQL && (w==L"EXEC"||w==L"EXECUTE"||w==L"PROCEDURE")) afterProcKw=true;
      i=j; continue;
    }
    // ---- 括号（彩虹着色）与标点 ----
    if(c==L'('||c==L'['||c==L'{'){ int col=(unsigned char)(depth%6); add(i,1,T_BRACKET,col); depth++; i++; continue; }
    if(c==L')'||c==L']'||c==L'}'){ int d=depth>0?depth-1:0; int col=(unsigned char)(d%6); add(i,1,T_BRACKET,col); if(depth>0)depth--; i++; continue; }
    if(c==L'{'||c==L'}'){ add(i,1,T_PUNCT,0); i++; continue; }
    if(c==L'.'){
      if(afterProcDot){ afterProcDot=false; afterProcKw=true; } // schema 之后真正的 proc 名
      add(i,1,T_PUNCT,0); i++; continue;
    }
    add(i,1,T_TEXT,0); i++; // 其它字符按普通文本处理
  }
}

// ----------------------------------------------------------------------------
// 加载文本
// ----------------------------------------------------------------------------
// 根据当前字号与屏幕 DPI 创建等宽字体，并测量出真实字符宽/行高。
// 用整串文本宽度求平均字符宽，比系统 tmAveCharWidth 更准确，避免长行偏移累积。
void ensureFont(){
  if(g_hFont) DeleteObject(g_hFont);
  HDC hdc=GetDC(g_hwnd);
  int h=-MulDiv(g_fontSize,GetDeviceCaps(hdc,LOGPIXELSY),72); // 字号(pt)转设备像素高度（取负表示字符高度）
  LOGFONT lf={0};
  lf.lfHeight=h; lf.lfWeight=FW_NORMAL; lf.lfCharSet=DEFAULT_CHARSET;
  wcscpy_s(lf.lfFaceName,L"Consolas");          // 首选 Consolas
  g_hFont=CreateFontIndirect(&lf);
  if(!g_hFont){ wcscpy_s(lf.lfFaceName,L"Lucida Console"); g_hFont=CreateFontIndirect(&lf); } // 回退1
  if(!g_hFont){ wcscpy_s(lf.lfFaceName,L"Courier New"); g_hFont=CreateFontIndirect(&lf); }    // 回退2
  SelectObject(hdc,g_hFont);
  TEXTMETRIC tm; GetTextMetrics(hdc,&tm);
  // 用整串宽度求平均字宽，比 tmAveCharWidth 更准，避免整行偏移累积
  const wchar_t* probe=L"the quick brown fox jumps over 0123456789";
  SIZE sz; GetTextExtentPoint32(hdc,probe,(int)wcslen(probe),&sz);
  g_charW = (int)((double)sz.cx/(double)wcslen(probe) + 0.5);
  if(g_charW<1) g_charW=tm.tmAveCharWidth;
  g_lineH = tm.tmHeight + tm.tmExternalLeading + 2;
  ReleaseDC(g_hwnd,hdc);
}

// 重新切分行为“逻辑行”数组，并用 scanLine(out=nullptr) 计算每一行起始的跨行状态
// （块注释/括号深度/ASPX 服务端块/块字符串引号），供后续 lineTokens 正确续行着色。
void rebuildLines(){
  g_lineStart.clear(); g_lineLen.clear(); g_lineDepth.clear();
  g_lineInBC.clear(); g_lineInSrv.clear(); g_lineInBlock.clear(); g_lineBsQ.clear();
  g_tokens.clear(); g_tokDone.clear();
  int n=(int)g_text.size();
  int i=0; int line=0;
  g_lineStart.push_back(0); // 第 0 行从偏移 0 开始
  for(i=0;i<n;i++){
    if(g_text[i]==L'\n'){
      g_lineLen.push_back(i-g_lineStart[line]); // 记录当前行长度（不含 \n）
      line++; g_lineStart.push_back(i+1);       // 下一行从 \n 之后开始
    }
  }
  g_lineLen.push_back(n - g_lineStart.back()); // 最后一行（可能没有末尾换行）
  g_lineCount = (int)g_lineStart.size();
  g_lineDepth.assign(g_lineCount,0);
  g_lineInBC.assign(g_lineCount,false);
  g_lineInSrv.assign(g_lineCount,false);
  g_lineBsQ.assign(g_lineCount,0);
  g_lineInBlock.assign(g_lineCount,0);
  bool inBC=false, inSrv=false; int inBlock=0; wchar_t bsQ=0; int depth=0;
  for(int l=0;l<g_lineCount;l++){
    g_lineDepth[l]=depth; g_lineInBC[l]=inBC; g_lineInSrv[l]=inSrv; g_lineInBlock[l]=inBlock; g_lineBsQ[l]=bsQ; // 写入本行起始状态
    int st=g_lineStart[l], en=st+g_lineLen[l];
    scanLine(g_text.c_str()+st, en-st, g_langId, inBC, inBlock, inSrv, bsQ, depth, nullptr); // 推进状态到行末
  }
  g_tokens.assign(g_lineCount, std::vector<Token>()); // 重置 token 缓存
  g_tokDone.assign(g_lineCount,false);
}

// 获取第 l 行的 token 列表；若尚未解析则懒解析（利用该行已存好的起始状态续行着色）。
const std::vector<Token>& lineTokens(int l){
  if(!g_tokDone[l]){
    bool inBC=g_lineInBC[l]; int inBlock=g_lineInBlock[l]; bool inSrv=g_lineInSrv[l]; wchar_t bsQ=g_lineBsQ[l]; int depth=g_lineDepth[l];
    g_tokens[l].clear();
    int st=g_lineStart[l], en=st+g_lineLen[l];
    scanLine(g_text.c_str()+st, en-st, g_langId, inBC, inBlock, inSrv, bsQ, depth, &g_tokens[l]);
    g_tokDone[l]=true;
  }
  return g_tokens[l];
}

// ----------------------------------------------------------------------------
// 视觉行（自动换行）
// 把一个“逻辑行”按可用宽度拆成多个“视觉行”，写入 g_visual。
// 不换行时一行对应一个视觉行；换行时按字符累计宽度切分。
// ----------------------------------------------------------------------------
void buildVisual(){
  g_visual.clear();
  int clientW = 0;
  if(g_hwnd){ RECT r; GetClientRect(g_hwnd,&r); clientW=r.right-r.left; }
  int avail = g_wrap ? (clientW - leftBar() - g_gutterW) : 0; // 换行模式下的可用文本宽度（像素）
  if(avail<g_charW) avail=g_charW;
  for(int l=0;l<g_lineCount;l++){
    int len=g_lineLen[l];
    int pw=linePx(l);
    if(!g_wrap || pw<=avail){
      g_visual.push_back({l,0,len}); // 整行作为一个视觉行
    } else {
      int start=0;
      while(start<len){
        int col=start, ww=0;
        while(col<len){
          int cw=charW(g_text[g_lineStart[l]+col]);
          if(ww+cw>avail && col>start) break; // 超过可用宽度则在此处断行（至少放一个字符）
          ww+=cw; col++;
        }
        g_visual.push_back({l,start,col-start}); // 视觉行：所属逻辑行、起始列、长度
        start=col;
      }
    }
  }
  g_visualCount=(int)g_visual.size();
}

// ----------------------------------------------------------------------------
// 括号配对
// 在光标处若处于一个括号字符上，则向同方向（前/后）寻找匹配的括号（用栈深度匹配）。
// 结果写入 g_matchA / g_matchB（分别为左右括号的字符偏移），供绘制高亮。
// ----------------------------------------------------------------------------
void findMatch(){
  g_matchA=-1; g_matchB=-1;
  if(g_caretOff<0||g_caretOff>=(int)g_text.size()) return;
  wchar_t c=g_text[g_caretOff];
  // 字符括号：() [] {} 必须“同类型”配对（修正大中小括号混合嵌套时把 [ 与 ) 配错的问题）
  static const wchar_t pairs[3][2]={{L'(',L')'},{L'[',L']'},{L'{',L'}'}};
  for(int p=0;p<3;p++){
    if(c==pairs[p][0]){ // 光标在左括号：向右找同类型右括号
      int stk=1;
      for(int i=g_caretOff+1;i<(int)g_text.size();i++){
        if(g_text[i]==pairs[p][0]) stk++;
        else if(g_text[i]==pairs[p][1]){ stk--; if(stk==0){ g_matchA=g_caretOff; g_matchAw=1; g_matchB=i; g_matchBw=1; return; } }
      }
      return;
    }
    if(c==pairs[p][1]){ // 光标在右括号：向左找同类型左括号
      int stk=1;
      for(int i=g_caretOff-1;i>=0;i--){
        if(g_text[i]==pairs[p][1]) stk++;
        else if(g_text[i]==pairs[p][0]){ stk--; if(stk==0){ g_matchA=i; g_matchAw=1; g_matchB=g_caretOff; g_matchBw=1; return; } }
      }
      return;
    }
  }
  // 词括号：SQL 的 BEGIN / END —— 整词匹配，按栈配对，像 () 一样高亮对应块的起止
  if(g_langId==L_SQL){
    int ws,we; wordAtOffset(g_caretOff,ws,we);
    if(we>ws){
      std::wstring w=g_text.substr(ws,we-ws);
      if(w==L"BEGIN"||w==L"END"||w==L"CASE"){
        // 判断某偏移所在整词：BEGIN 返回 1，END 返回 -1，其它返回 0
        auto wordKind=[&](int pos)->int{
          if(pos<0||pos>=(int)g_text.size()) return 0;
          int a=pos; while(a>0 && isWordChar(g_text[a-1])) a--;
          int b=pos; while(b<(int)g_text.size() && isWordChar(g_text[b])) b++;
          std::wstring ww=g_text.substr(a,b-a);
          if(ww==L"BEGIN"||ww==L"CASE") return 1;
          if(ww==L"END")   return -1;
          return 0;
        };
        if(w==L"BEGIN"){ // 向右找匹配的 END（自身已占一个 open，栈初值 1）
          int stk=1;
          for(int i=we;i<(int)g_text.size();){
            while(i<(int)g_text.size() && !isWordChar(g_text[i])) i++;
            if(i>=(int)g_text.size()) break;
            int a=i; while(i<(int)g_text.size() && isWordChar(g_text[i])) i++;
            int k=wordKind(a);
            if(k==1) stk++;
            else if(k==-1){ stk--; if(stk==0){ g_matchA=ws; g_matchAw=we-ws; int be=a; while(be<(int)g_text.size()&&isWordChar(g_text[be]))be++; g_matchB=a; g_matchBw=be-a; return; } }
          }
        } else { // END 向左找匹配的 BEGIN（自身已占一个 close，栈初值 1）
          int stk=1;
          for(int i=ws-1;i>=0;){
            while(i>=0 && !isWordChar(g_text[i])) i--;
            if(i<0) break;
            int b=i; while(b>=0 && isWordChar(g_text[b])) b--; b++;
            int k=wordKind(b);
            if(k==-1) stk++;
            else if(k==1){ stk--; if(stk==0){ int be=b; while(be<(int)g_text.size()&&isWordChar(g_text[be]))be++; g_matchA=b; g_matchAw=be-b; g_matchB=ws; g_matchBw=we-ws; return; } }
            i=b-1;
          }
        }
      }
    }
  }
}

// ----------------------------------------------------------------------------
// 滚动 / 光标
// ----------------------------------------------------------------------------
// 设置光标位置 off，并做三件事：
//   1) 更新括号配对；
//   2) 自动滚动使光标可见（垂直定位到 g_topLine，水平定位到 g_scrollX）；
//   3) 重绘 + 更新系统光标位置。
void setCaret(int off){
  g_caretOff=off;
  if(g_caretOff<0)g_caretOff=0;
  if(g_caretOff>(int)g_text.size())g_caretOff=(int)g_text.size();
  findMatch();
  int line=lineOfOffset(g_caretOff);
  int vline=-1;
  // 找到光标所在（按精确字符范围匹配）的视觉行
  for(int v=0;v<g_visualCount;v++){ if(g_visual[v].line==line && g_caretOff>=g_lineStart[line] && g_caretOff<=g_lineStart[line]+g_lineLen[line]){ vline=v; break; } }
  if(vline<0){ for(int v=0;v<g_visualCount;v++) if(g_visual[v].line==line){ vline=v; break; } } // 退而求其次按行找
  RECT r; GetClientRect(g_hwnd,&r); int edH=r.bottom-editorTop(); int visH=edH/g_lineH; // 可见视觉行数
  if(vline>=0){
    // 垂直滚动：若光标在可见区上方则顶到它，在下方则翻页使其可见
    if(vline<g_topLine) g_topLine=vline;
    else if(vline>=g_topLine+visH) g_topLine=vline-visH+1;
  }
  int col=g_caretOff-g_lineStart[line];
  int px=linePrefixPx(line,col);      // 光标在行内的像素 x
  int caretX = leftBar()+g_gutterW + px - g_scrollX; // 屏幕坐标 x
  if(caretX < leftBar()+g_gutterW) g_scrollX = px;            // 光标跑到左侧行号区外 => 左移
  else if(caretX > r.right-20) g_scrollX = leftBar()+g_gutterW + px - (r.right-20); // 右侧溢出 => 右移
  if(g_scrollX<0)g_scrollX=0;
  updateScroll();
  InvalidateRect(g_hwnd,NULL,TRUE); // 触发重绘
  updateCaretPos();                 // 移动系统光标
}
// 根据当前 g_caretOff 计算光标在屏幕上的 (x,y)，并创建/移动系统插入符（caret）。
void updateCaretPos(){
  if(!g_hwnd) return;
  int line=lineOfOffset(g_caretOff);
  int col=g_caretOff-g_lineStart[line];
  int vline=-1;
  for(int v=0;v<g_visualCount;v++){ if(g_visual[v].line==line && g_caretOff>=g_lineStart[line] && g_caretOff<=g_lineStart[line]+g_lineLen[line]){ vline=v; break; } }
  if(vline<0){ for(int v=0;v<g_visualCount;v++) if(g_visual[v].line==line){ vline=v; break; } }
  if(vline<0) return;
  int y=editorTop()+(vline-g_topLine)*g_lineH; // 屏幕 y（含标签栏/查找条偏移）
  int x;
  if(g_wrap){ x = leftBar()+g_gutterW + linePrefixPx(line, g_caretOff-g_lineStart[line]) - linePrefixPx(line, g_visual[vline].col); }
  else { x = leftBar()+g_gutterW + linePrefixPx(line, g_caretOff-g_lineStart[line]) - g_scrollX; } // 非换行按水平滚动偏移
  DestroyCaret();
  if(CreateCaret(g_hwnd,NULL,2,g_lineH-2)){
    SetCaretPos(x,y); ShowCaret(g_hwnd);
  }
}
// 根据当前文本行数与可见高度，重设垂直/水平滚动条的范围与位置。
void updateScroll(){
  RECT r; GetClientRect(g_hwnd,&r);
  int edH=r.bottom-editorTop(); if(edH<1)edH=1;
  int visH=edH/g_lineH;
  SCROLLINFO si={sizeof(si)};
  si.fMask=SIF_ALL; si.nMin=0; si.nMax=g_visualCount-1; si.nPage=visH; si.nPos=g_topLine;
  SetScrollInfo(g_hwnd,SB_VERT,&si,TRUE);
  int maxX=0;
  for(int l=0;l<g_lineCount;l++){ int w=linePx(l); if(w>maxX)maxX=w; } // 最长行的像素宽
  int clientW=r.right-r.left;
  int hmax=maxX-(clientW-leftBar()-g_gutterW); // 最大水平滚动量 = 最长行宽 - 可视文本宽
  if(hmax<0)hmax=0;
  SCROLLINFO hi={sizeof(hi)};
  hi.fMask=SIF_ALL; hi.nMin=0; hi.nMax=hmax; hi.nPage=clientW-g_gutterW; hi.nPos=g_scrollX;
  SetScrollInfo(g_hwnd,SB_HORZ,&hi,TRUE);
}

// ----------------------------------------------------------------------------
// 绘制
// ----------------------------------------------------------------------------
// ----------------------------------------------------------------------------
// 左侧文件夹浏览器（VSCode 风格）
// 说明：保持“单 cpp / 零依赖 / 全自绘”风格，侧栏树完全自绘（不引入原生 TreeView 控件）。
//       目录子项按需懒加载；目录先于文件、各自按名称排序；点击文件即以新标签打开。
// ----------------------------------------------------------------------------
struct TreeNode {
  std::wstring name;          // 显示名
  std::wstring fullPath;      // 完整路径
  bool isDir=false;           // 是否为文件夹
  bool expanded=false;        // 文件夹是否展开
  bool loaded=false;          // 子项是否已枚举
  int  depth=0;               // 缩进层级（根的直接子项=0）
  std::vector<TreeNode> children;
};
TreeNode g_treeRoot;                 // 根文件夹节点
bool    g_folderOpen=false;         // 是否已打开文件夹（决定侧栏是否显示）
std::wstring g_folderPath;          // 根文件夹路径
std::vector<TreeNode*> g_treeRows;  // 当前可见行（已展开层级扁平化后的指针列表）
int g_treeScroll=0;                 // 树垂直滚动像素
int g_sidebarHover=-1;              // 侧栏悬停行索引
int g_sideHoverBtn=0;               // 侧栏头部按钮悬停（1=关闭×）
bool g_treeDrag=false;              // 是否正在拖动树滚动条滑块
int  g_treeDragGrab=0;              // 拖动时鼠标与滑块顶部的偏移

// 左侧栏占用的宽度：未打开文件夹时为 0，编辑器与行号区占满整个客户区
int leftBar(){ return g_folderOpen?SIDEBAR_W:0; }

// 枚举目录子项：先文件夹后文件，各自按名称排序，结果写入 node.children
void loadDir(TreeNode& node){
  node.children.clear();
  WIN32_FIND_DATA fd;
  HANDLE h=FindFirstFile((node.fullPath+L"\\*").c_str(),&fd);
  if(h==INVALID_HANDLE_VALUE){ node.loaded=true; return; }
  std::vector<TreeNode> dirs, files;
  do{
    if(wcscmp(fd.cFileName,L".")==0||wcscmp(fd.cFileName,L"..")==0) continue;
    TreeNode c;
    c.name=fd.cFileName;
    c.fullPath=node.fullPath+L"\\"+fd.cFileName;
    c.isDir=(fd.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=0;
    c.depth=node.depth+1;
    if(c.isDir) dirs.push_back(c); else files.push_back(c);
  } while(FindNextFile(h,&fd));
  FindClose(h);
  auto cmp=[](const TreeNode&a,const TreeNode&b){ return a.name<b.name; };
  std::sort(dirs.begin(),dirs.end(),cmp);
  std::sort(files.begin(),files.end(),cmp);
  node.children.insert(node.children.end(),dirs.begin(),dirs.end());
  node.children.insert(node.children.end(),files.begin(),files.end());
  node.loaded=true;
}
// 递归把已展开目录的子项压入可见行列表
void pushTreeRows(TreeNode& node){
  for(auto& c: node.children){
    g_treeRows.push_back(&c);
    if(c.isDir && c.expanded) pushTreeRows(c);
  }
}
// 重建可见行：根的直接子项 + 所有已展开目录的子项
void rebuildTreeRows(){
  g_treeRows.clear();
  for(auto& c: g_treeRoot.children){ g_treeRows.push_back(&c); if(c.isDir && c.expanded) pushTreeRows(c); }
}
// 限制树滚动范围（不越界）
void clampTreeScroll(){
  RECT r; GetClientRect(g_hwnd,&r);
  int sbTop=editorTop()+SIDEBAR_HEAD_H;
  int viewH=r.bottom-sbTop; if(viewH<0)viewH=0;
  int contentH=(int)g_treeRows.size()*SIDEBAR_ROW_H;
  int maxS=contentH>viewH?contentH-viewH:0;
  if(g_treeScroll<0)g_treeScroll=0; if(g_treeScroll>maxS)g_treeScroll=maxS;
}
// 打开文件夹：载入根目录并展开，激活侧栏，编辑器整体右移让位
void openFolder(const std::wstring& path){
  if(path.empty()) return;
  g_folderPath=path;
  g_treeRoot=TreeNode();
  g_treeRoot.name=path.substr(path.find_last_of(L'\\')+1);
  if(g_treeRoot.name.empty()) g_treeRoot.name=path; // 驱动器根目录（如 C:\）时兜底显示完整路径
  g_treeRoot.fullPath=path; g_treeRoot.isDir=true; g_treeRoot.depth=-1; g_treeRoot.expanded=true;
  loadDir(g_treeRoot);
  g_folderOpen=true; rebuildTreeRows(); g_treeScroll=0; clampTreeScroll();
  buildVisual(); updateScroll();
  InvalidateRect(g_hwnd,NULL,TRUE);
}
// 关闭文件夹：隐藏侧栏，编辑器占满
void closeFolder(){
  g_folderOpen=false; g_treeRows.clear(); g_treeScroll=0; g_sidebarHover=-1; g_sideHoverBtn=0;
  buildVisual(); updateScroll();
  InvalidateRect(g_hwnd,NULL,TRUE);
}
// 文件夹选择对话框（SHBrowseForFolder）
std::wstring openFolderDialog(){
  BROWSEINFO bi={0};
  bi.hwndOwner=g_hwnd; bi.lpszTitle=L"选择要打开的文件夹";
  bi.ulFlags=BIF_RETURNONLYFSDIRS|BIF_NEWDIALOGSTYLE;
  LPITEMIDLIST pidl=SHBrowseForFolder(&bi);
  if(!pidl) return L"";
  wchar_t buf[MAX_PATH]={0};
  if(!SHGetPathFromIDList(pidl,buf)){ CoTaskMemFree(pidl); return L""; }
  CoTaskMemFree(pidl);
  return std::wstring(buf);
}
// 小图标：文件夹
void drawFolderIcon(HDC hdc,int x,int y,COLORREF col){
  HPEN p=CreatePen(PS_SOLID,1,col); HPEN op=(HPEN)SelectObject(hdc,p);
  HBRUSH b=CreateSolidBrush(col); HBRUSH ob=(HBRUSH)SelectObject(hdc,b);
  POINT pts[6];
  pts[0].x=x;    pts[0].y=y+3;
  pts[1].x=x+4;  pts[1].y=y+3;
  pts[2].x=x+6;  pts[2].y=y+5;
  pts[3].x=x+13; pts[3].y=y+5;
  pts[4].x=x+13; pts[4].y=y+13;
  pts[5].x=x;    pts[5].y=y+13;
  Polygon(hdc,pts,6);
  SelectObject(hdc,op); DeleteObject(p);
  SelectObject(hdc,ob); DeleteObject(b);
}
// 小图标：文件（带折角）
void drawFileIcon(HDC hdc,int x,int y,COLORREF col){
  HPEN p=CreatePen(PS_SOLID,1,col); HPEN op=(HPEN)SelectObject(hdc,p);
  HBRUSH b=CreateSolidBrush(col); HBRUSH ob=(HBRUSH)SelectObject(hdc,b);
  Rectangle(hdc,x,y,x+11,y+13);
  POINT pts[3];
  pts[0].x=x+6; pts[0].y=y; pts[1].x=x+11; pts[1].y=y; pts[2].x=x+11; pts[2].y=y+5;
  Polygon(hdc,pts,3);
  SelectObject(hdc,op); DeleteObject(p);
  SelectObject(hdc,ob); DeleteObject(b);
}
// 小图标：展开/折叠三角
void drawTreeTri(HDC hdc,int x,int y,bool down,COLORREF col){
  HPEN p=CreatePen(PS_SOLID,1,col); HPEN op=(HPEN)SelectObject(hdc,p);
  HBRUSH b=CreateSolidBrush(col); HBRUSH ob=(HBRUSH)SelectObject(hdc,b);
  POINT pts[3];
  if(down){ pts[0].x=x; pts[0].y=y-3; pts[1].x=x+7; pts[1].y=y-3; pts[2].x=x+3; pts[2].y=y+3; }
  else    { pts[0].x=x; pts[0].y=y-3; pts[1].x=x+6; pts[1].y=y;  pts[2].x=x;   pts[2].y=y+3; }
  Polygon(hdc,pts,3);
  SelectObject(hdc,op); DeleteObject(p);
  SelectObject(hdc,ob); DeleteObject(b);
}
// 侧栏鼠标按下：关闭按钮 / 滚动条滑块与轨道 / 行点击（目录展开折叠、文件打开）
void sidebarDown(int x,int y){
  RECT r; GetClientRect(g_hwnd,&r);
  int lb=leftBar();
  // 头部关闭按钮 ×
  RECT cr={lb-22, editorTop()+6, lb-4, editorTop()+6+18};
  POINT ptc={x,y};
  if(PtInRect(&cr,ptc)){ closeFolder(); return; }
  // 头部以下才是树区（滚动条与行点击均不响应头部区域）
  int sbTop=editorTop()+SIDEBAR_HEAD_H;
  if(y<sbTop) return;
  // 滚动条滑块 / 轨道
  int viewH=r.bottom-sbTop; int contentH=(int)g_treeRows.size()*SIDEBAR_ROW_H;
  if(contentH>viewH){
    int maxScroll=contentH-viewH; if(maxScroll<1)maxScroll=1;
    int thumbH=std::max(20,(int)((double)viewH/contentH*viewH));
    int thumbY=sbTop+(int)((double)g_treeScroll/maxScroll*(viewH-thumbH));
    RECT tr={lb-11, thumbY, lb-2, thumbY+thumbH};
    POINT pt={x,y};
    if(PtInRect(&tr,pt)){ g_treeDrag=true; g_treeDragGrab=y-thumbY; SetCapture(g_hwnd); return; }
    if(x>=lb-11 && x<=lb-2){ // 点击轨道：上/下翻页
      if(y<thumbY) g_treeScroll-=viewH; else g_treeScroll+=viewH;
      clampTreeScroll(); InvalidateRect(g_hwnd,NULL,TRUE); return;
    }
  }
  // 行点击
  int row=(y-sbTop+g_treeScroll)/SIDEBAR_ROW_H;
  if(row>=0 && row<(int)g_treeRows.size()){
    TreeNode* n=g_treeRows[row];
    if(n->isDir){ if(!n->loaded) loadDir(*n); n->expanded=!n->expanded; rebuildTreeRows(); clampTreeScroll(); InvalidateRect(g_hwnd,NULL,TRUE); }
    else { openInNewTab(n->fullPath); }
  }
}
// 侧栏鼠标移动：更新悬停行 / 头部按钮，触发高亮重绘
void sidebarMove(int x,int y){
  int lb=leftBar();
  RECT cr={lb-22, editorTop()+6, lb-4, editorTop()+6+18};
  POINT pt={x,y}; bool onClose=PtInRect(&cr,pt)!=0;
  int row=-1;
  if(!onClose){ int sbTop=editorTop()+SIDEBAR_HEAD_H; row=(y-sbTop+g_treeScroll)/SIDEBAR_ROW_H; if(row<0||row>=(int)g_treeRows.size()) row=-1; }
  if((onClose?(1):(0))!=g_sideHoverBtn || row!=g_sidebarHover){
    g_sideHoverBtn=onClose?1:0; g_sidebarHover=row;
    InvalidateRect(g_hwnd,NULL,TRUE);
  }
}
// 绘制整个侧栏（背景 / 头部 / 树行 / 分隔线 / 滚动条）
void drawSidebar(HDC mem, const RECT& rc){
  if(!g_folderOpen) return;
  int lb=leftBar();
  int eTop=editorTop();
  // 背景（从标签栏下沿 TAB_H 起，覆盖查找条左侧的留白带，避免缝隙）
  COLORREF sbBg = g_dark?0x002B2521:0x00F3F3F3;
  RECT sbrc={0,TAB_H,lb,rc.bottom};
  HBRUSH bb=CreateSolidBrush(sbBg); FillRect(mem,&sbrc,bb); DeleteObject(bb);
  // 头部
  RECT hdr={0,eTop,lb,eTop+SIDEBAR_HEAD_H};
  HBRUSH hb=CreateSolidBrush(g_dark?0x001F1B18:0x00ECECEC); FillRect(mem,&hdr,hb); DeleteObject(hb);
  SetBkMode(mem,TRANSPARENT);
  std::wstring fn=g_folderPath.substr(g_folderPath.find_last_of(L'\\')+1);
  if(fn.empty()) fn=g_folderPath; // 驱动器根目录时兜底显示完整路径
  RECT tr={8,eTop,lb-24,eTop+SIDEBAR_HEAD_H};
  SetTextColor(mem, g_dark?0x9DA5B4:0x444444);
  DrawText(mem,fn.c_str(),(int)fn.size(),&tr,DT_LEFT|DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS|DT_NOPREFIX);
  RECT cr={lb-22,eTop+6,lb-4,eTop+6+18};
  SetTextColor(mem, (g_sideHoverBtn==1)?(g_dark?0xFFFFFF:0x000000):(g_dark?0x888888:0x999999));
  DrawText(mem,L"×",1,&cr,DT_CENTER|DT_SINGLELINE|DT_VCENTER|DT_NOPREFIX);
  // 树行
  int sbTop=eTop+SIDEBAR_HEAD_H;
  COLORREF folderCol = g_dark?0x7BC0E5:0x2D7DD2;
  COLORREF fileCol   = g_dark?0x9DA5B4:0x5A5A5A;
  for(size_t i=0;i<g_treeRows.size();i++){
    int y=sbTop+(int)i*SIDEBAR_ROW_H-g_treeScroll;
    if(y+SIDEBAR_ROW_H<sbTop) continue;
    if(y>rc.bottom) break;
    TreeNode* n=g_treeRows[i];
    bool active=(n->fullPath==g_filePath);
    bool hover=(g_sidebarHover==(int)i);
    if(active||hover){
      RECT rr={0,y,lb,y+SIDEBAR_ROW_H};
      HBRUSH hb2=CreateSolidBrush(active?(g_dark?0x00714709:0x00D2E7FF):(g_dark?0x002A2D2E:0x00E6E6E6));
      FillRect(mem,&rr,hb2); DeleteObject(hb2);
    }
    int indent=10+n->depth*SIDEBAR_INDENT;
    if(n->isDir){
      drawTreeTri(mem,indent,y+SIDEBAR_ROW_H/2,n->expanded, g_dark?0x9DA5B4:0x555555);
      drawFolderIcon(mem,indent+14,(y+(SIDEBAR_ROW_H-12)/2),folderCol);
    } else {
      drawFileIcon(mem,indent+14,(y+(SIDEBAR_ROW_H-13)/2),fileCol);
    }
    RECT nr={indent+14+16, y, lb-4, y+SIDEBAR_ROW_H};
    SetTextColor(mem, active?(g_dark?0xFFFFFF:0x000000):(g_dark?0xD0D0D0:0x222222));
    DrawText(mem,n->name.c_str(),(int)n->name.size(),&nr,DT_LEFT|DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS|DT_NOPREFIX);
  }
  if(g_treeRows.empty()){
    RECT er={8,sbTop+6,lb-8,sbTop+24};
    SetTextColor(mem, g_dark?0x666666:0x999999);
    DrawText(mem,L"（空文件夹）",-1,&er,DT_LEFT|DT_SINGLELINE|DT_NOPREFIX);
  }
  // 分隔线（侧栏与编辑器之间）
  HPEN sp=CreatePen(PS_SOLID,1,g_dark?0x000000:0xDADADA); HPEN op=(HPEN)SelectObject(mem,sp);
  MoveToEx(mem,lb,TAB_H,NULL); LineTo(mem,lb,rc.bottom); SelectObject(mem,op); DeleteObject(sp);
  // 树滚动条滑块
  int viewH=rc.bottom-sbTop; int contentH=(int)g_treeRows.size()*SIDEBAR_ROW_H;
  if(contentH>viewH){
    int maxScroll=contentH-viewH; if(maxScroll<1)maxScroll=1;
    int thumbH=std::max(20,(int)((double)viewH/contentH*viewH));
    int thumbY=sbTop+(int)((double)g_treeScroll/maxScroll*(viewH-thumbH));
    RECT thr={lb-11,thumbY,lb-2,thumbY+thumbH};
    HBRUSH tb=CreateSolidBrush(g_dark?0x00545454:0x00BFBFBF); FillRect(mem,&thr,tb); DeleteObject(tb);
  }
}

// 绘制顶部标签栏（各个文档标签 + 新建按钮），覆盖在最上方一行。
void drawTabBar(HDC mem, const RECT& rc){
  COLORREF stripBg = g_dark?0x252526:0xF0F0F0;
  HBRUSH sb=CreateSolidBrush(stripBg); FillRect(mem,&rc,sb); DeleteObject(sb);
  // 标签栏底部分隔线
  HPEN bp=CreatePen(PS_SOLID,1,g_dark?0x3A3A3A:0xD0D0D0); HPEN op=(HPEN)SelectObject(mem,bp);
  MoveToEx(mem,0,TAB_H-1,NULL); LineTo(mem,rc.right,TAB_H-1); SelectObject(mem,op); DeleteObject(bp);

  int n=(int)g_docs.size();
  for(int i=0;i<n;i++){
    int tx=TAB_X0 + i*TAB_W;
    RECT tr={tx,0,tx+TAB_W,TAB_H};
    bool act=(i==g_active);
    COLORREF tb = act? (g_dark?0x1E1E1E:0xFFFFFF) : (g_dark?0x2D2D2D:0xE4E4E4); // 激活标签背景更亮
    HBRUSH tbk=CreateSolidBrush(tb); FillRect(mem,&tr,tbk); DeleteObject(tbk);
    if(act){
      // 激活标签底部画一条强调色横线
      HPEN ap=CreatePen(PS_SOLID,2,g_dark?0x569CD6:0x1976D2); HPEN ao=(HPEN)SelectObject(mem,ap);
      MoveToEx(mem,tx,TAB_H-1,NULL); LineTo(mem,tx+TAB_W,TAB_H-1); SelectObject(mem,ao); DeleteObject(ap);
    }
    // 标签文字：文件名（取路径最后一段），末尾省略号
    std::wstring t = g_docs[i].filePath.empty()? L"未命名" : g_docs[i].filePath.substr(g_docs[i].filePath.find_last_of(L'\\')+1);
    RECT tr2={tx+6,0,tx+TAB_W-20,TAB_H};
    SetBkMode(mem,TRANSPARENT);
    SetTextColor(mem, act? (g_dark?0xFFFFFF:0x000000) : (g_dark?0xC0C0C0:0x555555));
    DrawText(mem,t.c_str(),(int)t.size(),&tr2,DT_LEFT|DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS|DT_NOPREFIX);
    // 关闭按钮 ×
    RECT cr={tx+TAB_W-18,4,tx+TAB_W-4,TAB_H-4};
    SetTextColor(mem, g_dark?0xAAAAAA:0x888888);
    DrawText(mem,L"×",1,&cr,DT_CENTER|DT_SINGLELINE|DT_VCENTER|DT_NOPREFIX);
  }
  // “新建标签”按钮（+）
  int px=TAB_X0 + n*TAB_W;
  RECT pr={px+2,2,px+2+PLUS_W,TAB_H-2};
  HBRUSH pb=CreateSolidBrush(g_dark?0x2D2D2D:0xE4E4E4); FillRect(mem,&pr,pb); DeleteObject(pb);
  SetBkMode(mem,TRANSPARENT); SetTextColor(mem, g_dark?0xFFFFFF:0x000000);
  DrawText(mem,L"+",1,&pr,DT_CENTER|DT_SINGLELINE|DT_VCENTER|DT_NOPREFIX);
  SetBkMode(mem,OPAQUE);
}

// 主绘制函数：双缓冲（先画到内存 DC，再 BitBlt 到屏幕），避免闪烁。
void paint(){
  PAINTSTRUCT ps; HDC hdc=BeginPaint(g_hwnd,&ps);
  RECT rc; GetClientRect(g_hwnd,&rc);
  int eTop=editorTop(); // 编辑区从标签栏（及可能的查找条）下方开始
  HDC mem=CreateCompatibleDC(hdc);
  HBITMAP bmp=CreateCompatibleBitmap(hdc,rc.right,rc.bottom);
  HBITMAP old=(HBITMAP)SelectObject(mem,bmp);
  HGDIOBJ oldFnt=SelectObject(mem,g_hFont);
  SetBkMode(mem,OPAQUE);

  COLORREF* pal = g_dark?C_DARK:C_LIGHT; // 选取对应主题调色板
  COLORREF bg=bgColor();
  COLORREF gb=gutterBg();

  // 整体背景
  HBRUSH bgBr=CreateSolidBrush(bg);
  FillRect(mem,&rc,bgBr); DeleteObject(bgBr);

  // 行号区背景
  RECT grc={leftBar(),eTop,leftBar()+g_gutterW,rc.bottom};
  HBRUSH gbBr=CreateSolidBrush(gb);
  FillRect(mem,&grc,gbBr); DeleteObject(gbBr);
  // 行号区/文本区分隔竖线
  HPEN pen=CreatePen(PS_SOLID,1,g_dark?0x333333:0xE1E4E8);
  HPEN op=(HPEN)SelectObject(mem,pen);
  MoveToEx(mem,leftBar()+g_gutterW,eTop,NULL); LineTo(mem,leftBar()+g_gutterW,rc.bottom);
  SelectObject(mem,op); DeleteObject(pen);

  int edH=rc.bottom-eTop; if(edH<0)edH=0;
  int visH=edH/g_lineH + 1; // 可见视觉行数（+1 容差，避免边界闪烁）
  int startV=g_topLine; if(startV<0)startV=0;

  // 行号（右对齐在行号区内）
  SetTextColor(mem,gutterFg());
  SetBkColor(mem,gb);
  wchar_t num[16];
  for(int v=startV; v<g_visualCount && v<startV+visH+2; v++){
    int y=eTop+(v-startV)*g_lineH;
    int lineNo=v+1;
    _snwprintf(num,15,L"%d",lineNo);
    int tw=(int)wcslen(num)*g_charW;
    RECT nr={leftBar()+g_gutterW-6-tw,y,leftBar()+g_gutterW-6,y+g_lineH};
    ExtTextOut(mem,leftBar()+g_gutterW-6-tw,y,ETO_CLIPPED|ETO_OPAQUE,&nr,num,(UINT)wcslen(num),NULL);
  }

  // 文本
  for(int v=startV; v<g_visualCount && v<startV+visH+2; v++){
    int y=eTop+(v-startV)*g_lineH;
    const Visual& vis=g_visual[v];
    int line=vis.line; int base=g_lineStart[line]+vis.col; int len=vis.len;
    // 取本行的着色 token；纯文本则无 token
    const std::vector<Token>& toks = (g_langId==L_TXT)? std::vector<Token>() : lineTokens(line);
    // 把 token 映射到一个“视觉段内逐字符”的颜色数组 ttype/col
    std::vector<unsigned char> ttype(len, T_TEXT);
    std::vector<unsigned char> tcol(len,0);
    if(g_langId!=L_TXT){
      // t.start 是行内列号（相对行首），映射到本视觉段(vis.col 起)的颜色数组下标
      // 用与 [vis.col, vis.col+len) 的重叠区间填充，正确处理换行跨段与边界
      for(const Token& t: toks){
        int c0=t.start, c1=t.start+t.len;
        int lo=std::max(c0,vis.col), hi=std::min(c1,vis.col+len);
        for(int c=lo;c<hi;c++){
          ttype[c-vis.col]=t.type; tcol[c-vis.col]=t.col;
        }
      }
    } else {
      // 纯文本模式：仍对括号做彩虹着色
      int depth=g_lineDepth[line];
      for(int k=0;k<len;k++){
        wchar_t c=g_text[base+k];
        if(c==L'('||c==L'['||c==L'{'){ ttype[k]=T_BRACKET; tcol[k]=(unsigned char)(depth%6); depth++; }
        else if(c==L')'||c==L']'||c==L'}'){ int d=depth>0?depth-1:0; ttype[k]=T_BRACKET; tcol[k]=(unsigned char)(d%6); if(depth>0)depth--; }
      }
    }
    int x = (g_wrap)? (leftBar()+g_gutterW) : (leftBar()+g_gutterW - g_scrollX + linePrefixPx(line, vis.col));
    // 文本裁剪限制在整个可视行区域，不再用段宽矩形（段宽取整会裁掉末尾 1~2 字符）
    RECT lineClip={leftBar()+g_gutterW, y, rc.right, y+g_lineH};
    int k=0;
    // 把连续同色、同选中/配对状态的字符合并成一段绘制（减少 GDI 调用）
    while(k<len){
      unsigned char ty=ttype[k]; unsigned char tc=tcol[k];
      bool sel = (g_selStart>=0 && ((base+k)>=g_selStart && (base+k)<g_selEnd));
      bool mt  = ((g_matchA>=0 && (base+k)>=g_matchA && (base+k)<g_matchA+g_matchAw) ||
                  (g_matchB>=0 && (base+k)>=g_matchB && (base+k)<g_matchB+g_matchBw));
      bool mk  = (base+k < (int)g_markFlag.size()) ? (g_markFlag[base+k]!=0) : false; // 分词高亮命中
      int j=k+1;
      while(j<len){
        bool sel2=(g_selStart>=0 && ((base+j)>=g_selStart && (base+j)<g_selEnd));
        bool mt2=((g_matchA>=0 && (base+j)>=g_matchA && (base+j)<g_matchA+g_matchAw) ||
                  (g_matchB>=0 && (base+j)>=g_matchB && (base+j)<g_matchB+g_matchBw));
        bool mk2=(base+j < (int)g_markFlag.size()) ? (g_markFlag[base+j]!=0) : false;
        if(sel2!=sel || mt2!=mt || mk2!=mk || ttype[j]!=ty || tcol[j]!=tc) break;
        j++;
      }
      int segLen=j-k;
      COLORREF fg = (ty==T_BRACKET)? (g_dark?RB_DARK[tc]:RB_LIGHT[tc]) : pal[ty]; // 括号用彩虹色
      COLORREF bk = bg;
      if(mt) bk=matchBg();
      else if(sel) bk=selBg();
      else if(mk) bk=markBg();
      // 选中/配对：用真实字符像素宽填背景
      int runW = runPx(base+k, segLen);
      if(sel||mt){
        RECT segR={x-1, y, x+runW+1, y+g_lineH};
        HBRUSH hb=CreateSolidBrush(bk);
        FillRect(mem,&segR,hb); DeleteObject(hb);
      }
      SetTextColor(mem, sel? (g_dark?0xFFFFFF:0x000000) : fg);
      SetBkColor(mem,bk);
      ExtTextOut(mem,x,y,ETO_CLIPPED,&lineClip, g_text.c_str()+base+k, segLen, NULL);
      x+=runW;
      k=j;
    }
  }

  // 查找条（显示时画在标签栏下方的工具条区域，承载输入框背景与自绘按钮）
  if(g_hFind){
    int by=TAB_H, bh=FIND_H;
    RECT fbr={leftBar(),by,rc.right,by+bh};
    COLORREF fb = g_dark?0x002B2521:0x00F3F3F3;        // 工具条背景
    HBRUSH fbk=CreateSolidBrush(fb); FillRect(mem,&fbr,fbk); DeleteObject(fbk);
    HPEN sp=CreatePen(PS_SOLID,1,g_dark?0x3A3A3A:0xD0D0D0); HPEN sop=(HPEN)SelectObject(mem,sp);
    MoveToEx(mem,0,by+bh-1,NULL); LineTo(mem,rc.right,by+bh-1); // 底部分隔线
    SelectObject(mem,sop); DeleteObject(sp);
    // 放大镜图标（圆 + 手柄）
    int mx=leftBar()+g_gutterW+10, my=by+bh/2, rad=6;
    HPEN ip=CreatePen(PS_SOLID,2,g_dark?0x9DA5B4:0x555555); HPEN iop=(HPEN)SelectObject(mem,ip);
    HBRUSH ib=(HBRUSH)GetStockObject(NULL_BRUSH); HBRUSH ibo=(HBRUSH)SelectObject(mem,ib);
    Ellipse(mem,mx-rad,my-rad,mx+rad,my+rad);
    MoveToEx(mem,mx+rad-2,my+rad-2,NULL); LineTo(mem,mx+rad+3,my+rad+3);
    SelectObject(mem,iop); DeleteObject(ip); SelectObject(mem,ibo);
    // 三个自绘按钮
    drawFindButton(mem,g_rPrev,L"上一项", g_findHover==1, g_findPress==1, g_dark);
    drawFindButton(mem,g_rNext,L"下一项", g_findHover==2, g_findPress==2, g_dark);
    drawFindCloseBtn(mem,g_rClose, g_findHover==3, g_findPress==3, g_dark);
  }

  // 标签栏（覆盖最上方）
  RECT tbrc={0,0,rc.right,TAB_H};
  drawTabBar(mem,tbrc);

  // 左侧文件夹浏览器（覆盖在行号区左侧，与编辑器互不重叠）
  if(g_folderOpen) drawSidebar(mem,rc);

  // 把内存 DC 内容一次性拷贝到屏幕
  SelectObject(mem,oldFnt);
  BitBlt(hdc,0,0,rc.right,rc.bottom,mem,0,0,SRCCOPY);
  SelectObject(mem,old);
  DeleteObject(bmp);
  DeleteDC(mem);
  EndPaint(g_hwnd,&ps);
}

// ----------------------------------------------------------------------------
// 文件读取（编码识别）
// 支持 UTF-8(BOM/无BOM)、UTF-16 LE(BOM)、UTF-16 BE(BOM)、以及 GBK/系统 ANSI 回退。
// ----------------------------------------------------------------------------
std::wstring decodeBytes(const std::vector<BYTE>& b){
  // UTF-8 BOM (EF BB BF)
  if(b.size()>=3 && b[0]==0xEF && b[1]==0xBB && b[2]==0xBF){
    g_enc=1;
    int n=(int)b.size()-3; std::string s((char*)b.data()+3,n);
    int wn=MultiByteToWideChar(CP_UTF8,0,s.c_str(),(int)s.size(),NULL,0);
    std::wstring w; w.resize(wn);
    MultiByteToWideChar(CP_UTF8,0,s.c_str(),(int)s.size(),&w[0],wn);
    return w;
  }
  // UTF-16 LE BOM (FF FE)
  if(b.size()>=2 && b[0]==0xFF && b[1]==0xFE){
    g_enc=2;
    std::wstring w((wchar_t*)(b.data()+2),(b.size()-2)/2); return w;
  }
  // UTF-16 BE BOM (FE FF)：字节序需翻转
  if(b.size()>=2 && b[0]==0xFE && b[1]==0xFF){
    g_enc=3;
    std::wstring w; int n=(int)b.size()-2; w.resize(n/2);
    for(int i=0;i<n/2;i++){ w[i]=(wchar_t)(b[2+2*i+1]<<8 | b[2+2*i]); }
    return w;
  }
  // 无 BOM：先尝试按 UTF-8 解码（严格模式 MB_ERR_INVALID_CHARS）
  int wn=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,(char*)b.data(),(int)b.size(),NULL,0);
  if(wn>0){
    g_enc=0;
    std::wstring w; w.resize(wn);
    MultiByteToWideChar(CP_UTF8,0,(char*)b.data(),(int)b.size(),&w[0],wn);
    return w;
  }
  // 失败则按系统 ANSI 代码页（中文 Windows 通常为 GBK）解码
  g_enc=4;
  int an=MultiByteToWideChar(CP_ACP,0,(char*)b.data(),(int)b.size(),NULL,0);
  std::wstring w; w.resize(an);
  MultiByteToWideChar(CP_ACP,0,(char*)b.data(),(int)b.size(),&w[0],an);
  return w;
}

// 编码回写：将 wstring 按指定编码（g_enc 语义）转回字节，保存时按原编码写盘，保留 BOM。
std::vector<BYTE> encodeBytes(const std::wstring& w, int enc){
  std::vector<BYTE> out;
  if(enc==2){ // UTF-16 LE + BOM
    BYTE bom[]={0xFF,0xFE}; out.insert(out.end(),bom,bom+2);
    out.insert(out.end(),(BYTE*)w.data(),(BYTE*)(w.data()+w.size()));
  } else if(enc==3){ // UTF-16 BE + BOM
    BYTE bom[]={0xFE,0xFF}; out.insert(out.end(),bom,bom+2);
    for(wchar_t c:w){ out.push_back((BYTE)(c>>8)); out.push_back((BYTE)c); }
  } else if(enc==1){ // UTF-8 + BOM
    BYTE bom[]={0xEF,0xBB,0xBF}; out.insert(out.end(),bom,bom+2);
    int n=WideCharToMultiByte(CP_UTF8,0,w.c_str(),(int)w.size(),NULL,0,NULL,NULL);
    std::string s; s.resize(n); WideCharToMultiByte(CP_UTF8,0,w.c_str(),(int)w.size(),&s[0],n,NULL,NULL);
    out.insert(out.end(),(BYTE*)s.data(),(BYTE*)s.data()+n);
  } else if(enc==4){ // ANSI / GBK（系统代码页）
    int n=WideCharToMultiByte(CP_ACP,0,w.c_str(),(int)w.size(),NULL,0,NULL,NULL);
    std::string s; s.resize(n); WideCharToMultiByte(CP_ACP,0,w.c_str(),(int)w.size(),&s[0],n,NULL,NULL);
    out.insert(out.end(),(BYTE*)s.data(),(BYTE*)s.data()+n);
  } else { // 0 = UTF-8 无 BOM
    int n=WideCharToMultiByte(CP_UTF8,0,w.c_str(),(int)w.size(),NULL,0,NULL,NULL);
    std::string s; s.resize(n); WideCharToMultiByte(CP_UTF8,0,w.c_str(),(int)w.size(),&s[0],n,NULL,NULL);
    out.insert(out.end(),(BYTE*)s.data(),(BYTE*)s.data()+n);
  }
  return out;
}

// 打开文件：读入字节 -> 解码为 wstring -> 重切行 -> 重算视觉行 -> 重置视图 -> 重绘。
void loadFile(const std::wstring& path){
  HANDLE h=CreateFile(path.c_str(),GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
  if(h==INVALID_HANDLE_VALUE) return;
  DWORD sz=GetFileSize(h,NULL);
  if(sz==INVALID_FILE_SIZE||sz>200*1024*1024){ CloseHandle(h); return; } // 超过 200MB 拒绝加载
  std::vector<BYTE> buf(sz);
  DWORD rd=0; ReadFile(h,buf.data(),sz,&rd,NULL); CloseHandle(h);
  g_filePath=path;
  g_text=decodeBytes(buf);
  g_markWord.clear(); g_markFlag.clear(); g_markRanges.clear(); // 文本变化，清除分词高亮
  g_langId=langFromName();
  rebuildLines();
  buildVisual();
  g_caretOff=0; g_anchorOff=0; g_selStart=-1; g_selEnd=-1;
  g_topLine=0; g_scrollX=0; g_dirty=false;
  if(g_hwnd){ ensureFont(); updateScroll(); InvalidateRect(g_hwnd,NULL,TRUE); updateCaretPos(); }
  setWindowTitle();
}

// 根据当前文件名与语言设置窗口标题。
void setWindowTitle(){
  std::wstring name=g_filePath.empty()?L"LiteReader":g_filePath.substr(g_filePath.find_last_of(L'\\')+1);
  std::wstring lang;
  switch(g_langId){ case L_CS:lang=L" C#";break; case L_SQL:lang=L" SQL";break; case L_HTML:lang=L" HTML";break;
    case L_JS:lang=L" JS";break; case L_JSON:lang=L" JSON";break; case L_PY:lang=L" Python";break; case L_CSS:lang=L" CSS";break;
    case L_C:lang=L" C";break; case L_CPP:lang=L" C++";break; case L_JAVA:lang=L" Java";break; case L_ASPX:lang=L" ASPX";break; case L_XML:lang=L" XML";break;
    default:lang=L" 文本"; }
  bool dirty = (g_active>=0 && g_active<(int)g_docs.size())? g_docs[g_active].dirty : g_dirty;
  SetWindowText(g_hwnd,(name+L" - LiteReader  ·"+lang+(dirty?L"  *":L"")).c_str());
}

// 轻量编辑：在光标处插入文本（若存在选区则先替换选区）。插入后重置选区、重切行、重绘、置脏标记。
void insertText(const std::wstring& s){
  int start=g_caretOff, end=g_caretOff;
  if(g_selStart>=0){ start=g_selStart; end=g_selEnd; }
  g_text.replace(start, end-start, s);
  g_caretOff=start+(int)s.size();
  g_selStart=-1; g_selEnd=-1; g_anchorOff=g_caretOff;
  g_markWord.clear(); g_markFlag.clear(); g_markRanges.clear(); // 文本变化，清除分词高亮
  g_matchA=-1; g_matchB=-1;
  rebuildLines(); buildVisual(); updateScroll();
  if(g_active>=0 && g_active<(int)g_docs.size()) g_docs[g_active].dirty=true;
  g_dirty=true; updateCaretPos(); InvalidateRect(g_hwnd,NULL,TRUE); setWindowTitle();
}
// 轻量编辑：删除字符。forward=true 删除光标后（Delete 键），false 删除光标前（Backspace 键）；有选区则删除选区。
void deleteChar(bool forward){
  int start=g_caretOff, end=g_caretOff;
  if(g_selStart>=0){ start=g_selStart; end=g_selEnd; }
  else if(forward){ if(g_caretOff<(int)g_text.size()) end=g_caretOff+1; else return; }
  else { if(g_caretOff>0) start=g_caretOff-1; else return; }
  if(start>=end) return;
  g_text.erase(start, end-start);
  g_caretOff=start;
  g_selStart=-1; g_selEnd=-1; g_anchorOff=g_caretOff;
  g_markWord.clear(); g_markFlag.clear(); g_markRanges.clear();
  g_matchA=-1; g_matchB=-1;
  rebuildLines(); buildVisual(); updateScroll();
  if(g_active>=0 && g_active<(int)g_docs.size()) g_docs[g_active].dirty=true;
  g_dirty=true; updateCaretPos(); InvalidateRect(g_hwnd,NULL,TRUE); setWindowTitle();
}
// 另存为对话框（保存时用）
std::wstring saveFileDialog(){
  OPENFILENAME ofn={sizeof(ofn)}; wchar_t buf[MAX_PATH]={0};
  ofn.hwndOwner=g_hwnd; ofn.lpstrFilter=L"文本/代码\0*.txt;*.cs;*.sql;*.html;*.htm;*.xml;*.js;*.json;*.css;*.py;*.pyw\0所有文件\0*.*\0";
  ofn.lpstrFile=buf; ofn.nMaxFile=MAX_PATH; ofn.Flags=OFN_OVERWRITEPROMPT|OFN_PATHMUSTEXIST;
  if(GetSaveFileName(&ofn)) return std::wstring(buf);
  return L"";
}
// 保存：按原编码写回磁盘；无路径时弹“另存为”。写成功后清除脏标记。
void saveFile(){
  if(g_filePath.empty()){ std::wstring p=saveFileDialog(); if(p.empty()) return; g_filePath=p; }
  std::vector<BYTE> buf=encodeBytes(g_text, g_enc);
  HANDLE h=CreateFile(g_filePath.c_str(),GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
  if(h==INVALID_HANDLE_VALUE) return;
  DWORD wr=0; WriteFile(h,buf.data(),(DWORD)buf.size(),&wr,NULL); CloseHandle(h);
  if(g_active>=0 && g_active<(int)g_docs.size()){ g_docs[g_active].filePath=g_filePath; g_docs[g_active].dirty=false; }
  g_dirty=false; setWindowTitle();
}

// ----------------------------------------------------------------------------
// 查找
// ----------------------------------------------------------------------------
// 显示/隐藏查找条。再次调用则隐藏（同时编辑区复位）。
// 说明：查找条不再使用原生 BUTTON（默认灰凸样式偏丑），改为在 paint() 中自绘圆角按钮，
//       这里只创建“输入框”子控件，并算好三个自绘按钮的命中矩形供点击/悬停判定。
// 查找输入框自绘子类：拦截回车键，直接在框内触发“下一个”查找并保留焦点。
// 单行 EDIT 控件没有 EN_RETURN 通知；直接按回车会被系统当成“激活默认按钮”，
// 导致焦点跳到主窗口（标题栏）且回车被吞掉。这里拦截 VK_RETURN 亲自处理即可避免。
static WNDPROC g_oldFindProc = NULL;
void doFind(bool forward);   // 前置声明（定义于其后）
void toggleFind();          // 前置声明（定义于其后）
LRESULT CALLBACK FindEditProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp){
  if(msg==WM_KEYDOWN){
    if(wp==VK_RETURN){ doFind(true); SetFocus(g_hFind); return 0; } // 回车 = 下一个（焦点留在输入框）
    if(wp==VK_ESCAPE){ toggleFind(); return 0; }                    // ESC 关闭查找条
  }
  return CallWindowProc(g_oldFindProc, hw, msg, wp, lp);
}

void toggleFind(){
  if(g_hFind){
    DestroyWindow(g_hFind); g_hFind=NULL; g_oldFindProc=NULL; g_findHover=0; g_findPress=0; // 隐藏：销毁输入框与查找条
    InvalidateRect(g_hwnd,NULL,TRUE);
    return;
  }
  int ew=240, eh=20, ey=TAB_H+5;
  int ex=leftBar()+g_gutterW+28; // 左侧留出放大镜图标位置（随侧栏右移）
  g_hFind=CreateWindow(L"EDIT",L"",WS_CHILD|WS_VISIBLE|WS_BORDER|ES_AUTOHSCROLL|ES_WANTRETURN,
                        ex,ey,ew,eh,g_hwnd,(HMENU)2001,g_hInst,NULL);
  // 子类化输入框：捕获回车/ESC 的 WM_KEYDOWN，避免焦点丢失到标题栏
  g_oldFindProc = (WNDPROC)SetWindowLongPtr(g_hFind, GWLP_WNDPROC, (LONG_PTR)FindEditProc);
  // 计算三个自绘按钮的命中矩形（位于输入框右侧）
  int btnY=TAB_H+4, btnH=22, btnW=64;
  int nx=ex+ew+8;
  g_rPrev  ={nx,         btnY, nx+btnW,         btnY+btnH};
  g_rNext  ={nx+btnW+6,  btnY, nx+btnW+6+btnW,  btnY+btnH};
  int cx   =nx+btnW*2+6+12;
  g_rClose ={cx,         btnY, cx+24,           btnY+btnH};
  // 启用 WM_MOUSELEAVE，便于鼠标离开窗口时清除悬停高亮
  TRACKMOUSEEVENT tme={sizeof(tme),TME_LEAVE,g_hwnd,0}; TrackMouseEvent(&tme);
  SetFocus(g_hFind);
  InvalidateRect(g_hwnd,NULL,TRUE);
}
// 执行查找：forward 为 true 向后找，false 向前找。命中后选中并移动光标。
void doFind(bool forward){
  if(!g_hFind) return;
  int n=GetWindowTextLength(g_hFind); if(n<=0) return;
  std::wstring q; q.resize(n+1); GetWindowText(g_hFind,&q[0],n+1); q.resize(n);
  int start=g_caretOff;
  if(forward){
    int p=(int)g_text.find(q, start);
    if(p<0) p=(int)g_text.find(q,0);          // 到末尾没找到则从头再找（循环）
    if(p>=0){ g_selStart=p; g_selEnd=p+(int)q.size(); setCaret(p+(int)q.size()); g_selStart=p; g_selEnd=p+(int)q.size(); }
  } else {
    int p=(int)g_text.rfind(q, start-(int)q.size()-1);
    if(p<0) p=(int)g_text.rfind(q, g_text.size()); // 向前没找到则从尾部向前找（循环）
    if(p>=0){ g_selStart=p; g_selEnd=p+(int)q.size(); setCaret(p); g_selStart=p; g_selEnd=p+(int)q.size(); }
  }
}

// ----------------------------------------------------------------------------
// 文件关联注册（HKCU，无需管理员）
// 把本程序写入“打开方式”列表，并注册为 .txt/.cs 等扩展名的候选打开程序。
// ----------------------------------------------------------------------------
void registerDefault(){
  std::wstring exePath(MAX_PATH,0);
  GetModuleFileName(NULL,&exePath[0],MAX_PATH); exePath.resize(wcslen(exePath.c_str()));
  HKEY hk;
  std::wstring base=L"Software\\Classes\\Applications\\LiteReader.exe";
  if(RegCreateKeyEx(HKEY_CURRENT_USER,base.c_str(),0,NULL,0,KEY_WRITE,NULL,&hk,NULL)==ERROR_SUCCESS){
    RegSetValueEx(hk,L"FriendlyAppName",0,REG_SZ,(BYTE*)L"LiteReader 代码阅读器",(DWORD)(wcslen(L"LiteReader 代码阅读器")+1)*2);
    HKEY hs;
    if(RegCreateKeyEx(hk,L"shell\\open\\command",0,NULL,0,KEY_WRITE,NULL,&hs,NULL)==ERROR_SUCCESS){
      std::wstring cmd=L"\""+exePath+L"\" \"%1\""; // 双击文件时用本程序打开，并把路径作为 %1 传入
      RegSetValueEx(hs,L"",0,REG_SZ,(BYTE*)cmd.c_str(),(DWORD)(wcslen(cmd.c_str())+1)*2);
      RegCloseKey(hs);
    }
    RegCloseKey(hk);
  }
  const wchar_t* exts[]={L".txt",L".cs",L".sql",L".html",L".htm",L".xml",L".js",L".json",L".css",L".py",L".pyw",L".c",L".h",L".cpp",L".hpp",L".java",L".aspx",NULL};
  for(int i=0;exts[i];i++){
    std::wstring ek=L"Software\\Classes\\"+std::wstring(exts[i])+L"\\OpenWithList\\LiteReader.exe";
    HKEY h2; RegCreateKeyEx(HKEY_CURRENT_USER,ek.c_str(),0,NULL,0,KEY_WRITE,NULL,&h2,NULL); if(h2){RegCloseKey(h2);}
  }
  MessageBox(g_hwnd,L"已注册到「打开方式」列表。\n右键文件 → 打开方式 → 选择 LiteReader 即可。",L"LiteReader",MB_OK|MB_ICONINFORMATION);
}

// ----------------------------------------------------------------------------
// 多标签操作
// ----------------------------------------------------------------------------
// 弹出“打开文件”对话框，返回选中的文件路径（取消则返回空串）。
std::wstring openFileDialog(){
  OPENFILENAME ofn={sizeof(ofn)}; wchar_t buf[MAX_PATH]={0};
  ofn.hwndOwner=g_hwnd; ofn.lpstrFilter=L"文本/代码\0*.txt;*.cs;*.sql;*.html;*.htm;*.xml;*.js;*.json;*.css;*.py;*.pyw\0所有文件\0*.*\0";
  ofn.lpstrFile=buf; ofn.nMaxFile=MAX_PATH; ofn.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST;
  if(GetOpenFileName(&ofn)) return std::wstring(buf);
  return L"";
}

// 在新标签中打开文件：已打开则切换；否则新建标签并载入。
void openInNewTab(const std::wstring& path){
  if(path.empty()) return;
  // 去重：已打开则切换
  for(size_t i=0;i<g_docs.size();i++){
    if(g_docs[i].filePath==path){ switchTab((int)i); return; }
  }
  if(g_active>=0) snapshotTo(g_active); // 先把当前标签状态存好
  g_docs.push_back(Doc());
  g_active=(int)g_docs.size()-1;
  loadFile(path);
  g_docs[g_active].filePath=g_filePath;
  InvalidateRect(g_hwnd,NULL,TRUE);
}

// 切换到标签 j：保存当前、恢复目标、重算布局、重绘。
void switchTab(int j){
  if(j==g_active || j<0 || j>=(int)g_docs.size()) return;
  snapshotTo(g_active);
  g_active=j;
  restoreFrom(j);
  g_markWord.clear(); g_markFlag.clear(); g_markRanges.clear(); // 切换标签，清除上一个文档的分词高亮
  buildVisual();
  updateScroll();
  InvalidateRect(g_hwnd,NULL,TRUE);
  updateCaretPos();
  setWindowTitle();
}

// 关闭标签 i：仅剩一个时清空内容；否则删除并切换到相邻标签。
void closeTab(int i){
  if(i<0 || i>=(int)g_docs.size()) return;
  g_markWord.clear(); g_markFlag.clear(); g_markRanges.clear(); // 关闭标签，清除分词高亮
  int n=(int)g_docs.size();
  if(n<=1){
    // 仅剩一个：清空
    g_text.clear(); g_lineStart.clear(); g_lineLen.clear(); g_lineDepth.clear();
    g_lineInBC.clear(); g_lineInSrv.clear(); g_lineInBlock.clear(); g_lineBsQ.clear(); g_tokens.clear(); g_tokDone.clear(); g_lineCount=0;
    g_filePath.clear(); g_lang=L"auto"; g_langId=L_AUTO;
    g_caretOff=0; g_anchorOff=0; g_selStart=-1; g_selEnd=-1; g_matchA=-1; g_matchB=-1; g_matchAw=1; g_matchBw=1;
    g_topLine=0; g_scrollX=0; g_visual.clear(); g_visualCount=0;
    g_docs[0]=Doc();
    rebuildLines(); buildVisual(); updateScroll(); InvalidateRect(g_hwnd,NULL,TRUE); updateCaretPos(); setWindowTitle();
    return;
  }
  if(i==g_active){
    int neighbor = (i>0)? i-1 : 1; // 关闭当前标签后，激活其左侧（或右侧）邻居
    g_docs.erase(g_docs.begin()+i);
    g_active = neighbor; if(g_active>=(int)g_docs.size()) g_active=(int)g_docs.size()-1; if(g_active<0) g_active=0;
    restoreFrom(g_active);
    buildVisual(); updateScroll(); InvalidateRect(g_hwnd,NULL,TRUE); updateCaretPos(); setWindowTitle();
  } else {
    g_docs.erase(g_docs.begin()+i);
    if(i<g_active) g_active--; // 删除的是当前标签之前的，索引需前移
  }
  InvalidateRect(g_hwnd,NULL,TRUE);
}

// ----------------------------------------------------------------------------
// 菜单
// ----------------------------------------------------------------------------
// 创建主菜单：文件 / 编辑 / 视图（含语言子菜单）。菜单项 id 与 WM_COMMAND 中对应。
void createMenu(HMENU& hMenu){
  hMenu=CreateMenu();
  HMENU hFile=CreatePopupMenu();
  AppendMenu(hFile,MF_STRING,1001,L"打开...\tCtrl+O");
  AppendMenu(hFile,MF_STRING,1007,L"打开文件夹...\tCtrl+Shift+O");
  AppendMenu(hFile,MF_STRING,1004,L"新建标签\tCtrl+T");
  AppendMenu(hFile,MF_STRING,1005,L"关闭标签\tCtrl+W");
  AppendMenu(hFile,MF_STRING,1006,L"保存\tCtrl+S");
  AppendMenu(hFile,MF_STRING,1008,L"关闭文件夹");
  AppendMenu(hFile,MF_STRING,1002,L"设为默认打开程序");
  AppendMenu(hFile,MF_SEPARATOR,0,NULL);
  AppendMenu(hFile,MF_STRING,1003,L"退出");
  AppendMenu(hMenu,MF_POPUP,(UINT_PTR)hFile,L"文件");

  HMENU hEdit=CreatePopupMenu();
  AppendMenu(hEdit,MF_STRING,1101,L"复制\tCtrl+C");
  AppendMenu(hEdit,MF_STRING,1102,L"全选\tCtrl+A");
  AppendMenu(hEdit,MF_STRING,1103,L"查找\tCtrl+F");
  AppendMenu(hMenu,MF_POPUP,(UINT_PTR)hEdit,L"编辑");

  HMENU hView=CreatePopupMenu();
  AppendMenu(hView,MF_STRING,1201,L"明暗主题");
  AppendMenu(hView,MF_STRING,1202,L"自动换行");
  AppendMenu(hView,MF_STRING,1203,L"字体 +");
  AppendMenu(hView,MF_STRING,1204,L"字体 -");
  HMENU hLang=CreatePopupMenu();
  AppendMenu(hLang,MF_STRING,1300,L"自动");
  AppendMenu(hLang,MF_STRING,1301,L"纯文本");
  AppendMenu(hLang,MF_STRING,1302,L"C#");
  AppendMenu(hLang,MF_STRING,1303,L"SQL");
  AppendMenu(hLang,MF_STRING,1304,L"HTML");
  AppendMenu(hLang,MF_STRING,1305,L"JavaScript");
  AppendMenu(hLang,MF_STRING,1306,L"JSON");
  AppendMenu(hLang,MF_STRING,1307,L"Python");
  AppendMenu(hLang,MF_STRING,1308,L"CSS");
  AppendMenu(hLang,MF_STRING,1309,L"C");
  AppendMenu(hLang,MF_STRING,1310,L"C++");
  AppendMenu(hLang,MF_STRING,1311,L"Java");
  AppendMenu(hLang,MF_STRING,1312,L"ASPX");
  AppendMenu(hLang,MF_STRING,1313,L"XML");
  AppendMenu(hView,MF_POPUP,(UINT_PTR)hLang,L"语言");
  AppendMenu(hMenu,MF_POPUP,(UINT_PTR)hView,L"视图");
}
// 菜单弹出前（WM_INITMENU）刷新勾选状态：当前语言项、自动换行项打勾。
void checkMenu(HMENU hMenu){
  UINT langMap[]={1300,1301,1302,1303,1304,1305,1306,1307,1308,1309,1310,1311,1312,1313};
  int idx=0;
  if(g_lang==L"auto")idx=0; else if(g_lang==L"txt")idx=1; else if(g_lang==L"csharp")idx=2;
  else if(g_lang==L"sql")idx=3; else if(g_lang==L"html")idx=4; else if(g_lang==L"js")idx=5;
  else if(g_lang==L"json")idx=6; else if(g_lang==L"python")idx=7; else if(g_lang==L"css")idx=8;
  else if(g_lang==L"c")idx=9; else if(g_lang==L"cpp")idx=10; else if(g_lang==L"java")idx=11; else if(g_lang==L"aspx")idx=12; else if(g_lang==L"xml")idx=13;
  HMENU hView=GetSubMenu(hMenu,2);   // 视图菜单是第 3 个（索引 2）
  HMENU hLang=GetSubMenu(hView,5);   // 语言子菜单在视图内第 6 个（索引 5）
  for(int i=0;i<14;i++) CheckMenuItem(hLang,langMap[i],(i==idx)?MF_CHECKED:MF_UNCHECKED);
  CheckMenuItem(hView,1202,g_wrap?MF_CHECKED:MF_UNCHECKED); // 自动换行勾选
}

// ----------------------------------------------------------------------------
// 主窗口过程
// 处理所有窗口消息：创建、绘制、滚动、鼠标、键盘、菜单、命令、销毁等。
// ----------------------------------------------------------------------------
LRESULT CALLBACK WndProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp){
  switch(msg){
    case WM_CREATE:{
      DragAcceptFiles(hwnd,TRUE); // 允许拖拽文件到窗口
      g_docs.push_back(Doc());
      g_active=0;
      ensureFont();
      buildVisual();
      updateScroll();
      return 0;
    }
    case WM_DROPFILES:{
      HDROP h=(HDROP)wp; wchar_t buf[MAX_PATH];
      if(DragQueryFile(h,0,buf,MAX_PATH)){ openInNewTab(buf); } // 取拖入的第一个文件
      DragFinish(h);
      return 0;
    }
    case WM_SIZE:{
      ensureFont();
      buildVisual();
      updateScroll();
      clampTreeScroll();        // 窗口尺寸变化后，侧栏树滚动范围需重新夹紧
      InvalidateRect(hwnd,NULL,TRUE);
      if(g_hFind) toggleFind(); // 窗口尺寸变化会导致子控件错位，先收起查找条
      return 0;
    }
    case WM_VSCROLL:{
      int pos=GetScrollPos(hwnd,SB_VERT);
      RECT r; GetClientRect(hwnd,&r); int page=(r.bottom-editorTop())/g_lineH;
      int m=LOWORD(wp);
      if(m==SB_LINEUP) pos--; else if(m==SB_LINEDOWN) pos++;
      else if(m==SB_PAGEUP) pos-=page; else if(m==SB_PAGEDOWN) pos+=page;
      else if(m==SB_THUMBTRACK) pos=HIWORD(wp); else if(m==SB_THUMBPOSITION) pos=HIWORD(wp);
      if(pos<0)pos=0; if(pos>g_visualCount-1)pos=g_visualCount-1;
      g_topLine=pos; SetScrollPos(hwnd,SB_VERT,pos,TRUE); InvalidateRect(hwnd,NULL,TRUE);
      return 0;
    }
    case WM_HSCROLL:{
      int pos=GetScrollPos(hwnd,SB_HORZ);
      int m=LOWORD(wp);
      if(m==SB_LINELEFT)pos-=g_charW*4; else if(m==SB_LINERIGHT)pos+=g_charW*4;
      else if(m==SB_PAGELEFT)pos-=80; else if(m==SB_PAGERIGHT)pos+=80;
      else if(m==SB_THUMBTRACK)pos=HIWORD(wp); else if(m==SB_THUMBPOSITION)pos=HIWORD(wp);
      if(pos<0)pos=0;
      int maxX=0; for(int l=0;l<g_lineCount;l++){int w=linePx(l); if(w>maxX)maxX=w;}
      RECT r; GetClientRect(hwnd,&r); int hmax=maxX-(r.right-r.left-leftBar()-g_gutterW); if(hmax<0)hmax=0;
      if(pos>hmax)pos=hmax;
      g_scrollX=pos; SetScrollPos(hwnd,SB_HORZ,pos,TRUE); InvalidateRect(hwnd,NULL,TRUE);
      return 0;
    }
    case WM_MOUSEWHEEL:{
      int sx=(int)(short)LOWORD(lp), sy=(int)(short)HIWORD(lp);
      POINT pt={sx,sy}; ScreenToClient(hwnd,&pt);
      // 光标在侧栏内：滚动文件夹树
      if(g_folderOpen && pt.x<leftBar() && pt.y>=editorTop()){
        int delta=GET_WHEEL_DELTA_WPARAM(wp);
        int lines=(delta/WHEEL_DELTA)*(-3);
        g_treeScroll += lines*SIDEBAR_ROW_H; clampTreeScroll();
        InvalidateRect(hwnd,NULL,TRUE); return 0;
      }
      int delta=GET_WHEEL_DELTA_WPARAM(wp);
      int lines=(delta/WHEEL_DELTA)*(-3); // 每格滚轮滚动 3 行（方向取反）
      int pos=g_topLine+lines;
      if(pos<0)pos=0; if(pos>g_visualCount-1)pos=g_visualCount-1;
      g_topLine=pos; SetScrollPos(hwnd,SB_VERT,pos,TRUE); InvalidateRect(hwnd,NULL,TRUE);
      return 0;
    }
    case WM_COPYDATA:{
      // 来自另一个已存在实例：把命令行传入的文件路径在新实例中打开，并激活窗口
      PCOPYDATASTRUCT pc=(PCOPYDATASTRUCT)lp;
      if(pc && pc->dwData==1 && pc->lpData){
        std::wstring path=(wchar_t*)pc->lpData;
        if(!path.empty()) openInNewTab(path);
        bringToFront(hwnd);
      }
      return TRUE;
    }
    case WM_INITMENU: checkMenu((HMENU)wp); return 0;
    case WM_PAINT: paint(); return 0;
    case WM_ERASEBKGND: return 1; // 由 paint 自己画背景，禁止系统擦除（避免闪烁）
    case WM_SETFOCUS: updateCaretPos(); return 0;
    case WM_KILLFOCUS: DestroyCaret(); return 0;
    case WM_LBUTTONDOWN:{
      int x=(int)LOWORD(lp), y=(int)HIWORD(lp);
      // 标签栏区域：判断点中“新建”按钮、关闭按钮还是切换标签
      if(y<TAB_H){
        int n=(int)g_docs.size();
        int px=TAB_X0 + n*TAB_W;
        RECT pr={px+2,2,px+2+PLUS_W,TAB_H-2};
        POINT pt={x,y};
        if(PtInRect(&pr,pt)){ std::wstring p=openFileDialog(); if(!p.empty()) openInNewTab(p); return 0; }
        for(int i=0;i<n;i++){
          int tx=TAB_X0 + i*TAB_W;
          if(x>=tx && x<tx+TAB_W){
            RECT cr={tx+TAB_W-18,4,tx+TAB_W-4,TAB_H-4};
            if(PtInRect(&cr,pt)){ closeTab(i); return 0; } // 点中关闭 ×
            switchTab(i); return 0;                        // 否则切换标签
          }
        }
        return 0;
      }
      // 左侧文件夹树区域（在标签栏下方、查找条/编辑器左侧）
      if(g_folderOpen && x<leftBar() && y>=editorTop()){ sidebarDown(x,y); return 0; }
      // 查找条区域（标签栏与编辑区之间）：命中自绘按钮或保持焦点在输入框
      if(g_hFind && x>=leftBar() && y>=TAB_H && y<TAB_H+FIND_H){
        POINT pt={x,y};
        if(PtInRect(&g_rClose,pt)){ g_findPress=3; toggleFind(); return 0; }
        if(PtInRect(&g_rPrev,pt)){ g_findPress=1; doFind(false); SetFocus(g_hFind); InvalidateRect(hwnd,NULL,TRUE); return 0; }
        if(PtInRect(&g_rNext,pt)){ g_findPress=2; doFind(true);  SetFocus(g_hFind); InvalidateRect(hwnd,NULL,TRUE); return 0; }
        SetFocus(g_hFind); return 0; // 点查找条其它处：焦点留在输入框，不动文本
      }
      if(x<leftBar()+g_gutterW) return 0; // 点中行号区或侧栏，忽略
      // 编辑器内单击：清除之前的分词高亮，避免残留
      if(!g_markWord.empty()){ g_markWord.clear(); g_markFlag.clear(); g_markRanges.clear(); }
      int ey=y-editorTop();
      int v=ey/g_lineH + g_topLine; // 由 y 反推视觉行
      if(v<0||v>=g_visualCount) return 0;
      const Visual& vis=g_visual[v];
      // 由 x 反推字符列：把像素偏移换算回列号（考虑换行模式与水平滚动）
      int target = (g_wrap? linePrefixPx(vis.line,vis.col) : 0) + (x-leftBar()-g_gutterW) + (g_wrap?0:g_scrollX);
      int col = pxToColAbs(vis.line, target);
      if(col<vis.col)col=vis.col;
      if(col>vis.col+vis.len)col=vis.col+vis.len;
      if(col<0)col=0; if(col>g_lineLen[vis.line])col=g_lineLen[vis.line];
      int off=g_lineStart[vis.line]+col; // 最终字符偏移
      SetFocus(hwnd);
      g_anchorOff=off; g_caretOff=off; g_selStart=-1; g_selEnd=-1;
      findMatch(); updateCaretPos(); InvalidateRect(hwnd,NULL,TRUE);
      SetCapture(hwnd); // 捕获鼠标以便拖拽选择
      return 0;
    }
    case WM_MOUSEMOVE:{
      int x=(int)LOWORD(lp), y=(int)HIWORD(lp);
      // 正在拖动侧栏树滚动条：实时更新滚动位置
      if(g_treeDrag){
        RECT r; GetClientRect(g_hwnd,&r);
        int sbTop=editorTop()+SIDEBAR_HEAD_H;
        int viewH=r.bottom-sbTop; int contentH=(int)g_treeRows.size()*SIDEBAR_ROW_H;
        if(contentH>viewH){
          int maxScroll=contentH-viewH; if(maxScroll<1)maxScroll=1;
          int thumbH=std::max(20,(int)((double)viewH/contentH*viewH));
          int thumbY=y-g_treeDragGrab;
          int newScroll=(int)((double)(thumbY-sbTop)/(viewH-thumbH)*maxScroll);
          g_treeScroll=newScroll; clampTreeScroll(); InvalidateRect(hwnd,NULL,TRUE);
        }
        return 0;
      }
      // 查找条按钮悬停态跟踪（仅编辑器区域内）
      if(g_hFind){
        int h=0; POINT pt={x,y};
        if(x>=leftBar() && y>=TAB_H && y<TAB_H+FIND_H){
          if(PtInRect(&g_rPrev,pt))h=1; else if(PtInRect(&g_rNext,pt))h=2; else if(PtInRect(&g_rClose,pt))h=3;
        }
        if(h!=g_findHover){ g_findHover=h; TRACKMOUSEEVENT tme={sizeof(tme),TME_LEAVE,g_hwnd,0}; TrackMouseEvent(&tme); RECT rc; GetClientRect(hwnd,&rc); RECT br={leftBar(),TAB_H,rc.right,TAB_H+FIND_H}; InvalidateRect(hwnd,&br,TRUE); }
      }
      // 侧栏悬停高亮（目录/文件行与关闭按钮）
      if(g_folderOpen && x<leftBar() && y>=editorTop()){ sidebarMove(x,y); return 0; }
      if(wp & MK_LBUTTON){
        if(y<TAB_H) return 0;
        // 选区拖到编辑区边缘时自动水平滚动
        RECT r; GetClientRect(hwnd,&r);
        int lb=leftBar();
        int maxX=0; for(int ll=0;ll<g_lineCount;ll++){int w=linePx(ll); if(w>maxX)maxX=w;}
        int hmax=maxX-(r.right-r.left-lb-g_gutterW); if(hmax<0)hmax=0;
        if(hmax>0 && x>=lb+g_gutterW){
          int step=g_charW*3; bool scrolled=false;
          if(x>r.right-24){ g_scrollX+=step; scrolled=true; }
          else if(x<lb+g_gutterW+24){ g_scrollX-=step; scrolled=true; }
          if(g_scrollX<0)g_scrollX=0; if(g_scrollX>hmax)g_scrollX=hmax;
          if(scrolled){ SetScrollPos(hwnd,SB_HORZ,g_scrollX,TRUE); }
        }
        int ey=y-editorTop();
        int v=ey/g_lineH + g_topLine;
        if(v<0)v=0; if(v>=g_visualCount)v=g_visualCount-1;
        const Visual& vis=g_visual[v];
        int target = (g_wrap? linePrefixPx(vis.line,vis.col) : 0) + (x-lb-g_gutterW) + (g_wrap?0:g_scrollX);
        int col = pxToColAbs(vis.line, target);
        if(col<vis.col)col=vis.col;
        if(col>vis.col+vis.len)col=vis.col+vis.len;
        if(col<0)col=0; if(col>g_lineLen[vis.line])col=g_lineLen[vis.line];
        int off=g_lineStart[vis.line]+col;
        g_caretOff=off;
        // 以 anchor 为起点、当前 off 为终点，生成选区 [selStart, selEnd)
        if(g_anchorOff<g_caretOff){ g_selStart=g_anchorOff; g_selEnd=g_caretOff; }
        else { g_selStart=g_caretOff; g_selEnd=g_anchorOff; }
        findMatch(); updateCaretPos(); InvalidateRect(hwnd,NULL,TRUE);
      }
      return 0;
    }
    case WM_LBUTTONUP:{
      if(g_treeDrag){ g_treeDrag=false; ReleaseCapture(); return 0; }
      if(g_findPress){ g_findPress=0; RECT rc; GetClientRect(hwnd,&rc); RECT br={0,TAB_H,rc.right,TAB_H+FIND_H}; InvalidateRect(hwnd,&br,TRUE); }
      ReleaseCapture(); return 0;
    }
    case WM_MOUSELEAVE:{
      // 鼠标离开窗口：清除查找条按钮与侧栏的悬停高亮
      if(g_findHover){ g_findHover=0; RECT rc; GetClientRect(hwnd,&rc); RECT br={leftBar(),TAB_H,rc.right,TAB_H+FIND_H}; InvalidateRect(hwnd,&br,TRUE); }
      if(g_sidebarHover!=-1 || g_sideHoverBtn){ g_sidebarHover=-1; g_sideHoverBtn=0; InvalidateRect(hwnd,NULL,TRUE); }
      return 0;
    }
    case WM_LBUTTONDBLCLK:{
      // 双击：把光标位置的“整词”选中，并高亮标记文档中所有相同分词
      int x=(int)LOWORD(lp), y=(int)HIWORD(lp);
      if(y<TAB_H) return 0;                                   // 标签栏不处理
      if(g_hFind && y<TAB_H+FIND_H) return 0;                 // 查找条不处理
      if(x<leftBar()+g_gutterW) return 0;
      int ey=y-editorTop();
      int v=ey/g_lineH + g_topLine;
      if(v<0||v>=g_visualCount) return 0;
      const Visual& vis=g_visual[v];
      int target=(g_wrap? linePrefixPx(vis.line,vis.col):0)+(x-leftBar()-g_gutterW)+(g_wrap?0:g_scrollX);
      int col=pxToColAbs(vis.line,target);
      if(col<vis.col)col=vis.col; if(col>vis.col+vis.len)col=vis.col+vis.len;
      if(col<0)col=0; if(col>g_lineLen[vis.line])col=g_lineLen[vis.line];
      int off=g_lineStart[vis.line]+col;
      int ws,we; wordAtOffset(off,ws,we);                     // 扩展为整词
      std::wstring w=g_text.substr(ws,we-ws);
      // SQL 的 BEGIN / END 视为块括号：双击只选中该词本身，不做全文档高亮
      // （避免一次选中所有 BEGIN 或所有 END），并触发括号配对高亮对应块。
      bool isSqlBlock = (g_langId==L_SQL && (w==L"BEGIN"||w==L"END"||w==L"CASE"));
      if(isSqlBlock){
        g_markWord.clear(); g_markFlag.clear(); g_markRanges.clear(); // 清除分词高亮
      } else {
        g_markWord = w;                                        // 记录标记词
        collectMarks();                                        // 收集所有整词命中
      }
      g_selStart=ws; g_selEnd=we; g_anchorOff=ws; g_caretOff=we; // 选中该分词
      findMatch(); updateCaretPos(); InvalidateRect(hwnd,NULL,TRUE);
      return 0;
    }
    case WM_KEYDOWN:{
      bool ctrl=(GetKeyState(VK_CONTROL)&0x8000)!=0;
      bool shift=(GetKeyState(VK_SHIFT)&0x8000)!=0;
      // 组合键：Ctrl+O/T/W/F/C/A 与 Ctrl+Tab 切换标签
      if(ctrl){
        if(wp=='O'){
          if(shift){ std::wstring p=openFolderDialog(); if(!p.empty()) openFolder(p); }
          else { std::wstring p=openFileDialog(); if(!p.empty()) openInNewTab(p); }
          return 0;
        }
        else if(wp=='T'){ std::wstring p=openFileDialog(); if(!p.empty()) openInNewTab(p); return 0; }
        else if(wp=='W'){ closeTab(g_active); return 0; }
        else if(wp=='F'){ toggleFind(); return 0; }
        else if(wp=='S'){ saveFile(); return 0; }
        else if(wp=='C'){
          if(g_selStart>=0){ std::wstring sub=g_text.substr(g_selStart,g_selEnd-g_selStart);
            HGLOBAL hg=GlobalAlloc(GMEM_MOVEABLE,(sub.size()+1)*2);
            wchar_t* p=(wchar_t*)GlobalLock(hg); wcscpy_s(p,sub.size()+1,sub.c_str()); GlobalUnlock(hg);
            OpenClipboard(hwnd); EmptyClipboard(); SetClipboardData(CF_UNICODETEXT,hg); CloseClipboard();
          }
          return 0;
        }
        else if(wp=='A'){ g_selStart=0; g_selEnd=(int)g_text.size(); g_anchorOff=0; g_caretOff=(int)g_text.size(); InvalidateRect(hwnd,NULL,TRUE); return 0; }
        else if(wp==VK_TAB){ int n=(int)g_docs.size(); if(n>1){ int nx= shift? (g_active-1+n)%n : (g_active+1)%n; switchTab(nx); } return 0; }
      }
      // 方向键/Home/End/PageUp/PageDown 移动光标（保持列号，遇短行则夹取）
      int line=lineOfOffset(g_caretOff);
      int col=g_caretOff-g_lineStart[line];
      int np=g_caretOff; // 新的光标偏移
      switch(wp){
        case VK_LEFT: np=g_caretOff-1; break;
        case VK_RIGHT: np=g_caretOff+1; break;
        case VK_UP:{
          if(col>0 && g_text[g_caretOff-1]==L'\n'){ np=g_caretOff-1; break; }
          if(line>0){ int pl=line-1; int pcol=std::min(col,(int)g_lineLen[pl]); np=g_lineStart[pl]+pcol; if(np>0&&g_text[np-1]==L'\n')np--; }
          break;
        }
        case VK_DOWN:{
          if(col<g_lineLen[line] && g_text[g_caretOff]==L'\n'){ np=g_caretOff+1; break; }
          if(line<g_lineCount-1){ int nl=line+1; int ncol=std::min(col,(int)g_lineLen[nl]); np=g_lineStart[nl]+ncol; if(np>0&&g_text[np-1]==L'\n')np--; }
          break;
        }
        case VK_HOME: np=g_lineStart[line]; break;
        case VK_END: np=g_lineStart[line]+g_lineLen[line]; break;
        case VK_PRIOR:{
          RECT r; GetClientRect(hwnd,&r); int page=(r.bottom-editorTop())/g_lineH;
          int v=-1; for(int q=0;q<g_visualCount;q++) if(g_visual[q].line==line){v=q;break;}
          v-=page; if(v<0)v=0; np=g_lineStart[g_visual[v].line]+std::min(col,(int)g_lineLen[g_visual[v].line]); break;
        }
        case VK_NEXT:{
          RECT r; GetClientRect(hwnd,&r); int page=(r.bottom-editorTop())/g_lineH;
          int v=-1; for(int q=0;q<g_visualCount;q++) if(g_visual[q].line==line){v=q;break;}
          v+=page; if(v>=g_visualCount)v=g_visualCount-1; np=g_lineStart[g_visual[v].line]+std::min(col,(int)g_lineLen[g_visual[v].line]); break;
        }
        case VK_F3: doFind(true); return 0; // F3 重复上次查找
        // 轻量编辑：退格/删除/回车/制表符（均作用于编辑器文本，交给编辑函数处理并保持光标）
        case VK_BACK:   deleteChar(false); return 0;
        case VK_DELETE: deleteChar(true);  return 0;
        case VK_RETURN: insertText(L"\n");  return 0;
        case VK_TAB:    insertText(L"\t");  return 0;
        default: return DefWindowProc(hwnd,msg,wp,lp); // 其它按键交还系统
      }
      if(np<0)np=0; if(np>(int)g_text.size())np=(int)g_text.size();
      g_caretOff=np;
      if(shift){ if(g_anchorOff<g_caretOff){g_selStart=g_anchorOff;g_selEnd=g_caretOff;} else {g_selStart=g_caretOff;g_selEnd=g_anchorOff;} }
      else { g_anchorOff=g_caretOff; g_selStart=-1; g_selEnd=-1; } // 无 Shift：取消选区，锚点跟随
      findMatch(); setCaret(np);
      return 0;
    }
    case WM_CHAR:{
      wchar_t ch=(wchar_t)wp;
      if(ch==0x7F){ deleteChar(true); return 0; }   // DEL
      if(ch<0x20) return 0;                          // 控制字符（回车/退格/Tab 已在 WM_KEYDOWN 处理）
      if(g_hFind && GetFocus()==g_hFind) return DefWindowProc(hwnd,msg,wp,lp); // 焦点在查找框时不当作编辑器输入
      insertText(std::wstring(1,ch));                // 普通字符：插入到光标处
      return 0;
    }
    case WM_COMMAND:{
      int id=LOWORD(wp);
      if(id==2001){
        // 查找输入框：回车/ESC 已由 FindEditProc 子类拦截处理（见 toggleFind）。
        // 单行 EDIT 无 EN_RETURN 通知，故不在此依赖 0x0300（实为 EN_CHANGE 文本变更），避免误触发。
      }
      else if(id==1001){ std::wstring p=openFileDialog(); if(!p.empty()) openInNewTab(p); }
      else if(id==1004){ std::wstring p=openFileDialog(); if(!p.empty()) openInNewTab(p); }
      else if(id==1005){ closeTab(g_active); }
      else if(id==1006){ saveFile(); }
      else if(id==1002){ registerDefault(); }
      else if(id==1003){ DestroyWindow(hwnd); }
      else if(id==1007){ std::wstring p=openFolderDialog(); if(!p.empty()) openFolder(p); }
      else if(id==1008){ closeFolder(); }
      else if(id==1101){ // 复制
        if(g_selStart>=0){ std::wstring sub=g_text.substr(g_selStart,g_selEnd-g_selStart);
          HGLOBAL hg=GlobalAlloc(GMEM_MOVEABLE,(sub.size()+1)*2);
          wchar_t* p=(wchar_t*)GlobalLock(hg); wcscpy_s(p,sub.size()+1,sub.c_str()); GlobalUnlock(hg);
          OpenClipboard(hwnd); EmptyClipboard(); SetClipboardData(CF_UNICODETEXT,hg); CloseClipboard();
        }
      }
      else if(id==1102){ g_selStart=0; g_selEnd=(int)g_text.size(); g_anchorOff=0; g_caretOff=(int)g_text.size(); InvalidateRect(hwnd,NULL,TRUE); }
      else if(id==1103){ toggleFind(); }
      else if(id==1201){ g_dark=!g_dark; InvalidateRect(hwnd,NULL,TRUE); } // 切换深浅主题
      else if(id==1202){ g_wrap=!g_wrap; buildVisual(); updateScroll(); InvalidateRect(hwnd,NULL,TRUE); }
      else if(id==1203){ g_fontSize++; if(g_fontSize>28)g_fontSize=28; ensureFont(); buildVisual(); updateScroll(); InvalidateRect(hwnd,NULL,TRUE); updateCaretPos(); }
      else if(id==1204){ g_fontSize--; if(g_fontSize<9)g_fontSize=9; ensureFont(); buildVisual(); updateScroll(); InvalidateRect(hwnd,NULL,TRUE); updateCaretPos(); }
      else if(id>=1300 && id<=1313){ // 选择语言
        const wchar_t* langs[]={L"auto",L"txt",L"csharp",L"sql",L"html",L"js",L"json",L"python",L"css",L"c",L"cpp",L"java",L"aspx",L"xml"};
        g_lang=langs[id-1300];
        g_langId=langFromName();
        g_tokens.assign(g_lineCount, std::vector<Token>()); g_tokDone.assign(g_lineCount,false); // 清缓存重着色
        // 同步到当前文档快照
        if(g_active>=0) g_docs[g_active].lang=g_lang, g_docs[g_active].langId=g_langId;
        InvalidateRect(hwnd,NULL,TRUE); setWindowTitle();
      }
      return 0;
    }
    case WM_DESTROY: PostQuitMessage(0); return 0;
  }
  return DefWindowProc(hwnd,msg,wp,lp);
}

// ----------------------------------------------------------------------------
// 关键字表
// 程序启动时调用一次，把各语言的关键字/类型名灌入对应 set。
// ----------------------------------------------------------------------------
void initKeywords(){
  const wchar_t* cs[]={L"abstract",L"as",L"base",L"break",L"case",L"catch",L"checked",L"class",L"const",L"continue",L"default",L"delegate",L"do",L"else",L"event",L"explicit",L"extern",L"finally",L"fixed",L"for",L"foreach",L"goto",L"if",L"implicit",L"in",L"interface",L"internal",L"is",L"lock",L"namespace",L"new",L"null",L"operator",L"out",L"override",L"params",L"private",L"protected",L"public",L"readonly",L"ref",L"return",L"sealed",L"sizeof",L"stackalloc",L"static",L"switch",L"this",L"throw",L"true",L"false",L"try",L"typeof",L"uint",L"ulong",L"unchecked",L"unsafe",L"using",L"virtual",L"volatile",L"while",L"add",L"async",L"await",L"init",L"record",L"required",L"scoped",L"unmanaged",L"when",L"with",L"yield",L"get",L"set",L"value",L"var",NULL};
  const wchar_t* cst[]={L"bool",L"byte",L"char",L"decimal",L"double",L"float",L"int",L"long",L"sbyte",L"short",L"uint",L"ulong",L"ushort",L"void",L"object",L"string",L"dynamic",L"nint",L"nuint",NULL};
  const wchar_t* sql[]={L"SELECT",L"FROM",L"WHERE",L"INSERT",L"UPDATE",L"DELETE",L"CREATE",L"TABLE",L"DROP",L"ALTER",L"ADD",L"COLUMN",L"INDEX",L"VIEW",L"FUNCTION",L"PROCEDURE",L"TRIGGER",L"DATABASE",L"SCHEMA",L"JOIN",L"INNER",L"LEFT",L"RIGHT",L"OUTER",L"FULL",L"CROSS",L"ON",L"AS",L"AND",L"OR",L"NOT",L"NULL",L"IS",L"IN",L"LIKE",L"BETWEEN",L"EXISTS",L"ANY",L"ALL",L"GROUP",L"BY",L"ORDER",L"HAVING",L"DISTINCT",L"UNION",L"INTERSECT",L"EXCEPT",L"INTO",L"VALUES",L"SET",L"PRIMARY",L"KEY",L"FOREIGN",L"REFERENCES",L"UNIQUE",L"DEFAULT",L"CHECK",L"CONSTRAINT",L"CASCADE",L"CASE",L"WHEN",L"THEN",L"ELSE",L"END",L"LIMIT",L"OFFSET",L"ASC",L"DESC",L"TOP",L"WITH",L"GRANT",L"REVOKE",L"BEGIN",L"COMMIT",L"ROLLBACK",L"TRANSACTION",L"DECLARE",L"IF",L"WHILE",L"LOOP",L"RETURN",L"CAST",L"CONVERT",L"COALESCE",L"COUNT",L"SUM",L"AVG",L"MIN",L"MAX",L"IDENTITY",L"ENGINE",L"CHARSET",L"COLLATE",L"EXEC",L"EXECUTE",NULL};
  const wchar_t* js[]={L"var",L"let",L"const",L"function",L"return",L"if",L"else",L"for",L"while",L"do",L"switch",L"case",L"break",L"continue",L"new",L"class",L"extends",L"super",L"this",L"typeof",L"instanceof",L"in",L"of",L"try",L"catch",L"finally",L"throw",L"await",L"async",L"yield",L"import",L"export",L"from",L"default",L"void",L"delete",L"null",L"undefined",L"true",L"false",L"NaN",L"arguments",L"get",L"set",L"static",NULL};
  const wchar_t* py[]={L"def",L"class",L"return",L"if",L"elif",L"else",L"for",L"while",L"break",L"continue",L"import",L"from",L"as",L"with",L"try",L"except",L"finally",L"raise",L"lambda",L"pass",L"global",L"nonlocal",L"yield",L"async",L"await",L"in",L"is",L"not",L"and",L"or",L"None",L"True",L"False",L"self",L"assert",L"del",L"print",NULL};
  const wchar_t* css[]={L"@import",L"@media",L"@keyframes",L"@font-face",L"@charset",L"@page",L"important",NULL};
  for(int i=0;cs[i];i++)KW_CS.insert(cs[i]);
  for(int i=0;cst[i];i++)TY_CS.insert(cst[i]);
  for(int i=0;sql[i];i++)KW_SQL.insert(sql[i]);
  for(int i=0;js[i];i++)KW_JS.insert(js[i]);
  for(int i=0;py[i];i++)KW_PY.insert(py[i]);
  for(int i=0;css[i];i++)KW_CSS.insert(css[i]);

  const wchar_t* ckw[]={L"auto",L"break",L"case",L"char",L"const",L"continue",L"default",L"do",L"double",L"else",L"enum",L"extern",L"float",L"for",L"goto",L"if",L"inline",L"int",L"long",L"register",L"restrict",L"return",L"short",L"signed",L"sizeof",L"static",L"struct",L"switch",L"typedef",L"union",L"unsigned",L"void",L"volatile",L"while",L"_Alignas",L"_Alignof",L"_Atomic",L"_Bool",L"_Complex",L"_Generic",L"_Imaginary",L"_Noreturn",L"_Static_assert",L"_Thread_local",NULL};
  const wchar_t* cppkw[]={L"asm",L"auto",L"bool",L"break",L"case",L"catch",L"char",L"class",L"const",L"const_cast",L"constexpr",L"continue",L"decltype",L"default",L"delete",L"do",L"double",L"dynamic_cast",L"else",L"enum",L"explicit",L"export",L"extern",L"false",L"float",L"for",L"friend",L"goto",L"if",L"inline",L"int",L"long",L"mutable",L"namespace",L"new",L"noexcept",L"nullptr",L"operator",L"private",L"protected",L"public",L"register",L"reinterpret_cast",L"return",L"short",L"signed",L"sizeof",L"static",L"static_assert",L"static_cast",L"struct",L"switch",L"template",L"this",L"throw",L"true",L"try",L"typedef",L"typeid",L"typename",L"union",L"unsigned",L"using",L"virtual",L"void",L"volatile",L"wchar_t",L"while",L"and",L"and_eq",L"bitand",L"bitor",L"compl",L"not",L"not_eq",L"or",L"or_eq",L"xor",L"xor_eq",L"final",L"override",L"concept",L"requires",L"co_await",L"co_return",L"co_yield",L"import",L"module",L"char8_t",L"char16_t",L"char32_t",L"thread_local",NULL};
  const wchar_t* javakw[]={L"abstract",L"assert",L"boolean",L"break",L"byte",L"case",L"catch",L"char",L"class",L"const",L"continue",L"default",L"do",L"double",L"else",L"enum",L"extends",L"final",L"finally",L"float",L"for",L"goto",L"if",L"implements",L"import",L"instanceof",L"int",L"interface",L"long",L"native",L"new",L"package",L"private",L"protected",L"public",L"return",L"short",L"static",L"strictfp",L"super",L"switch",L"synchronized",L"this",L"throw",L"throws",L"transient",L"try",L"void",L"volatile",L"while",L"var",L"true",L"false",L"null",NULL};
  const wchar_t* cty[]={L"int",L"char",L"float",L"double",L"void",L"long",L"short",L"unsigned",L"signed",L"size_t",L"ssize_t",L"int8_t",L"int16_t",L"int32_t",L"int64_t",L"uint8_t",L"uint16_t",L"uint32_t",L"uint64_t",L"wchar_t",L"ptrdiff_t",L"intptr_t",L"uintptr_t",L"FILE",L"bool",L"_Bool",NULL};
  const wchar_t* cppty[]={L"bool",L"wchar_t",L"char8_t",L"char16_t",L"char32_t",L"short",L"int",L"long",L"float",L"double",L"void",L"size_t",L"int8_t",L"int16_t",L"int32_t",L"int64_t",L"uint8_t",L"uint16_t",L"uint32_t",L"uint64_t",L"string",L"wstring",L"vector",L"map",L"unordered_map",L"set",L"unordered_set",L"list",L"deque",L"queue",L"stack",L"array",L"pair",L"tuple",L"shared_ptr",L"unique_ptr",L"weak_ptr",L"bitset",L"ostream",L"istream",L"iostream",L"ifstream",L"ofstream",L"cout",L"cin",L"cerr",L"endl",L"nullptr",NULL};
  const wchar_t* javaty[]={L"int",L"char",L"boolean",L"byte",L"short",L"long",L"float",L"double",L"void",L"String",L"Object",L"Integer",L"Boolean",L"Double",L"Float",L"Long",L"Short",L"Byte",L"Character",L"StringBuilder",L"StringBuffer",L"List",L"Map",L"Set",L"ArrayList",L"HashMap",L"HashSet",L"LinkedList",L"Exception",L"RuntimeException",L"Throwable",NULL};
  for(int i=0;ckw[i];i++)KW_C.insert(ckw[i]);
  for(int i=0;cppkw[i];i++)KW_CPP.insert(cppkw[i]);
  for(int i=0;javakw[i];i++)KW_JAVA.insert(javakw[i]);
  for(int i=0;cty[i];i++)TY_C.insert(cty[i]);
  for(int i=0;cppty[i];i++)TY_CPP.insert(cppty[i]);
  for(int i=0;javaty[i];i++)TY_JAVA.insert(javaty[i]);
}

// ----------------------------------------------------------------------------
// 图标：运行时用 GDI 程序化绘制 “LR” 字标（圆角蓝底 + 白色 LR），生成 HICON。
// 完全不引用任何外部图片文件，保持单 cpp 可编译。
// 解决“显示发虚/锯齿”的两个关键点：
//   1) 大图标按 256px 生成，任务栏/Alt+Tab 大尺寸时直接以 256 显示，不再把一张
//      32px 小图拉伸放大。
//   2) 等效“矢量”清晰度：先以 SS 倍超采样（SS=4/8）在高分辨率离屏缓冲绘制，再
//      平均下采样到目标尺寸。圆角与文字边缘得到平滑的半透明 alpha，而非硬 1bit
//      锯齿。这样图标在任意显示尺寸下都是“按尺寸重绘”，等效矢量。
// ----------------------------------------------------------------------------
// 在 sz×sz 的 32bpp DIB 上输出：圆角矩形底 + 居中 “LR” 文字，alpha 由覆盖度平滑决定。
HICON makeLRIcon(int sz){
  if(sz<=0) return NULL;
  const int SS = (sz>=128)?4:8;   // 超采样倍数：大图标 4x 已足够，小图标 8x 更精细
  const int R  = sz*SS;           // 高分辨率离屏缓冲边长

  // 1) 在高分辨率 32bpp DIB 上绘制
  BITMAPINFOHEADER bi={0};
  bi.biSize=sizeof(BITMAPINFOHEADER);
  bi.biWidth=R; bi.biHeight=R; bi.biPlanes=1; bi.biBitCount=32; bi.biCompression=BI_RGB;
  RGBQUAD* hb=NULL;
  HBITMAP hBmp=CreateDIBSection(NULL,(BITMAPINFO*)&bi,DIB_RGB_COLORS,(void**)&hb,NULL,0);
  if(!hBmp||!hb) return NULL;
  HDC hdc=CreateCompatibleDC(NULL);
  HBITMAP hOld=(HBITMAP)SelectObject(hdc,hBmp);
  memset(hb,0,(size_t)R*R*4); // 先清为透明（RGB=0, A=0）

  // 圆角矩形底（蓝色）
  int m=R/8;
  HBRUSH br=CreateSolidBrush(RGB(38,120,212));
  HPEN   pn=CreatePen(PS_NULL,0,0);
  HBRUSH ob=(HBRUSH)SelectObject(hdc,br);
  HPEN   op=(HPEN)SelectObject(hdc,pn);
  RoundRect(hdc,m,m,R-m,R-m,R/3,R/3);
  SelectObject(hdc,ob); SelectObject(hdc,op); DeleteObject(br); DeleteObject(pn);

  // “LR” 文字（白色、粗体）
  LOGFONT lf={0}; lf.lfHeight=-(int)(R*0.5); lf.lfWeight=FW_BOLD; lf.lfCharSet=DEFAULT_CHARSET;
  wcscpy_s(lf.lfFaceName,L"Segoe UI");
  HFONT f=CreateFontIndirect(&lf); HFONT of=(HFONT)SelectObject(hdc,f);
  SetBkMode(hdc,TRANSPARENT); SetTextColor(hdc,RGB(255,255,255));
  RECT tr={0,0,R,R}; DrawText(hdc,L"LR",2,&tr,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
  SelectObject(hdc,of); DeleteObject(f);

  // 2) 超采样下采样：把 R×R 平均到 sz×sz，得到带平滑 alpha 边缘的图标
  BITMAPINFOHEADER bi2={0};
  bi2.biSize=sizeof(BITMAPINFOHEADER);
  bi2.biWidth=sz; bi2.biHeight=sz; bi2.biPlanes=1; bi2.biBitCount=32; bi2.biCompression=BI_RGB;
  RGBQUAD* bits=NULL;
  HBITMAP hBmp2=CreateDIBSection(NULL,(BITMAPINFO*)&bi2,DIB_RGB_COLORS,(void**)&bits,NULL,0);
  if(!hBmp2||!bits){ SelectObject(hdc,hOld); DeleteDC(hdc); DeleteObject(hBmp); return NULL; }
  memset(bits,0,(size_t)sz*sz*4);
  const int SS2=SS*SS;
  for(int y=0;y<sz;y++){
    for(int x=0;x<sz;x++){
      unsigned long aR=0,aG=0,aB=0,cov=0;
      for(int sy=0;sy<SS;sy++){
        const RGBQUAD* row=hb+(size_t)(y*SS+sy)*R;
        for(int sx=0;sx<SS;sx++){
          const RGBQUAD& p=row[x*SS+sx];
          if(p.rgbRed||p.rgbGreen||p.rgbBlue){ cov++; aR+=p.rgbRed; aG+=p.rgbGreen; aB+=p.rgbBlue; }
        }
      }
      RGBQUAD& d=bits[(size_t)y*sz+x];
      if(cov){
        d.rgbRed   = (BYTE)(aR/cov);
        d.rgbGreen = (BYTE)(aG/cov);
        d.rgbBlue  = (BYTE)(aB/cov);
        d.rgbReserved = (BYTE)((cov*255)/SS2); // 覆盖度 => 平滑半透明 alpha
      }
    }
  }
  SelectObject(hdc,hOld); DeleteDC(hdc); DeleteObject(hBmp);

  // 单色遮罩：全 0 => 由颜色位图 alpha 决定透明度
  int rowBytes=((sz+15)/16)*2;
  std::vector<BYTE> mask((size_t)rowBytes*sz,0);
  HBITMAP hMask=CreateBitmap(sz,sz,1,1,mask.data());

  ICONINFO ii={0}; ii.fIcon=TRUE; ii.hbmColor=hBmp2; ii.hbmMask=hMask;
  HICON h=CreateIconIndirect(&ii);
  DeleteObject(hBmp2); DeleteObject(hMask); // CreateIconIndirect 已拷贝像素
  return h;
}
// 为窗口设置大/小两套图标（标题栏与任务栏显示 “LR”）
void setAppIcon(HWND hw){
  // 大图标用 256px：标题栏/任务栏/Alt+Tab 大尺寸时直接以 256 显示，不再被拉伸模糊
  // 小图标用 32px：由 Windows 按需下采样到 16/24，比直接给 16 在高分屏更稳更清晰
  HICON big=makeLRIcon(256), small=makeLRIcon(32);
  if(big)   SendMessage(hw,WM_SETICON,ICON_BIG,(LPARAM)big);
  if(small) SendMessage(hw,WM_SETICON,ICON_SMALL,(LPARAM)small);
}

// ----------------------------------------------------------------------------
// 入口
// ----------------------------------------------------------------------------
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow){
  g_hInst = hInstance;
  CoInitializeEx(NULL, COINIT_APARTMENTTHREADED|COINIT_DISABLE_OLE1DDE); // 供 SHBrowseForFolder 使用
  initKeywords(); // 先灌入关键字表

  // ---- 单实例：若已存在窗口，把命令行文件发给它并激活，不另开进程 ----
  HANDLE hMutex=CreateMutexW(NULL,TRUE,L"LiteReaderSingleInstance_v1");
  bool other=(GetLastError()==ERROR_ALREADY_EXISTS);
  if(other){
    HWND hw=NULL;
    for(int t=0;t<60 && !hw;t++){ hw=FindWindowW(WNDCLASS_NAME,NULL); if(!hw) Sleep(20); } // 轮询等待目标窗口出现
    if(hw){
      int argc; LPWSTR* argv=CommandLineToArgvW(GetCommandLineW(),&argc);
      for(int a=1;a<argc;a++){
        COPYDATASTRUCT cds; cds.dwData=1;
        cds.cbData=(UINT)((wcslen(argv[a])+1)*sizeof(wchar_t));
        cds.lpData=(PVOID)argv[a];
        SendMessageW(hw,WM_COPYDATA,(WPARAM)NULL,(LPARAM)&cds); // 通过 WM_COPYDATA 把路径传给已有实例
      }
      if(argv) LocalFree(argv);
      bringToFront(hw);
      CloseHandle(hMutex);
      CoUninitialize();
      return 0; // 自己退出，由已有实例打开文件
    }
  }

  WNDCLASS wc={0};
  wc.style=CS_DBLCLKS; // 启用双击消息（WM_LBUTTONDBLCLK），用于双击选词/高亮
  wc.lpfnWndProc=WndProc; wc.hInstance = hInstance; wc.hCursor=LoadCursor(NULL,IDC_IBEAM);
  wc.lpszClassName=WNDCLASS_NAME; wc.hbrBackground=(HBRUSH)GetStockObject(WHITE_BRUSH);
  RegisterClass(&wc);
  g_hwnd=CreateWindowEx(WS_EX_ACCEPTFILES,WNDCLASS_NAME,L"LiteReader",
    WS_OVERLAPPEDWINDOW|WS_VSCROLL|WS_HSCROLL,
    CW_USEDEFAULT,CW_USEDEFAULT,900,640,NULL,NULL,hInstance,NULL);
  HMENU hMenu; createMenu(hMenu); SetMenu(g_hwnd,hMenu);
  setAppIcon(g_hwnd); // 运行时绘制 “LR” 图标（不引用任何图片文件，保持单 cpp）
  ShowWindow(g_hwnd, nCmdShow);
  UpdateWindow(g_hwnd);

  // 命令行参数 = 要打开的文件（载入首个标签）
  int argc; LPWSTR* argv=CommandLineToArgvW(GetCommandLineW(),&argc);
  if(argc>=2){ loadFile(argv[1]); g_docs[0].filePath=g_filePath; }
  else { setWindowTitle(); }
  if(argv) LocalFree(argv);

  // 标准 Win32 消息循环
  MSG msg;
  while(GetMessage(&msg,NULL,0,0)){ TranslateMessage(&msg); DispatchMessage(&msg); }
  CoUninitialize();
  return 0;
}
