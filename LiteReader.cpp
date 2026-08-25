// LiteReader - 轻量级原生代码阅读器 (Win32 / C++)
// 零依赖，静态编译，单 exe。支持语法高亮、彩虹括号、行号、平滑选择、多编码、多标签。
// 作者注：整个程序只有一个 .cpp 文件 + 一个 .cpp 编译出的 .exe，无需任何第三方库。
#define WIN32_LEAN_AND_MEAN // 只引入最小化的 Win32 头，减少编译体积
#define NOMINMAX            // 禁止 windows.h 把 min/max 定义为不安全宏，改用自有实现
#define _CRT_SECURE_NO_WARNINGS // 允许使用 _snwprintf 等（MSVC 标记为弃用，MinGW 无此限制；须在包含 <cstdio> 前定义）
#define _WIN32_WINNT 0x0501 // 启用 TrackMouseEvent 等 XP+ API
#include <windows.h>        // Win32 API 核心（窗口、消息、GDI）
#include <commdlg.h>        // 通用对话框（打开文件对话框 OPENFILENAME）
#include <shellapi.h>       // 拖拽文件支持（DragAcceptFiles / HDROP）
#include <string>           // std::wstring 等
#include <vector>           // 动态数组（文本行、token、文档等）
#include <set>              // 关键字集合（O(log n) 查找）
#include <algorithm>        // 保留头文件以兼容其他 std 算法（本项目已改用自有 min/max）
#include <map>              // std::map（行渲染缓存）
#include <unordered_map>    // std::unordered_map（token 懒缓存，避免大文件预分配海量空 vector）
#include <cwctype>          // iswupper / iswalnum / iswdigit（宽字符版）
#include <cwchar>           // 宽字符处理
#include <cstdio>           // _snwprintf 等
#include <shlobj.h>         // SHBrowseForFolder / SHGetPathFromIDList（打开文件夹对话框）
#include <objbase.h>        // CoInitializeEx / CoTaskMemFree

// 自有 min/max：windows.h 在 NOMINMAX 未定义时会把 min/max 当成不安全宏展开；
// 这里自行实现，不再依赖 std::max/std::min。模板保留类型推导，行为与其一致。
template <typename T> inline T min(T a, T b) {
    return a < b ? a : b;
}
template <typename T> inline T max(T a, T b) {
    return a > b ? a : b;
}

// ----------------------------------------------------------------------------
// 标签栏布局常量
// ----------------------------------------------------------------------------
const int TAB_H = 26;       // 标签栏高度（像素）
const int TAB_W = 160;      // 标签默认宽度（现改为按名称动态计算，此常量仅作后备）
const int TAB_X0 = 0;       // 第一个标签起始 x 坐标
const int PLUS_W = 24;      // 右上角“新建标签”按钮宽度（固定钉在最右）
const int TAB_W_MIN = 120;  // 单个标签最小宽度（像素）
const int TAB_W_MAX = 320;  // 单个标签最大宽度（像素），超过则末尾省略号兜底
const int TAB_CLOSE_W = 18; // 标签内关闭按钮宽度（像素）
const int TAB_PAD = 12;     // 标签文字左右内边距合计（左6 + 右6），用于动态宽度估算
const int MENU_H = 24;      // 顶部自绘菜单栏高度（像素）

// 左侧文件夹浏览器（VSCode 风格）布局常量
const int SIDEBAR_W = 240;         // 侧栏默认宽度（像素）；未打开文件夹时为 0
const int SIDEBAR_W_MIN = 140;     // 侧栏可调最小宽度
const int SIDEBAR_W_MAX = 560;     // 侧栏可调最大宽度
const int SIDEBAR_RESIZE_AREA = 5; // 侧栏右缘拖动调宽命中宽度（像素）
const int SIDEBAR_ROW_H = 22;      // 树每行高度
const int SIDEBAR_HEAD_H = 30;     // 侧栏顶部标题栏高度
const int SIDEBAR_INDENT = 16;     // 每级缩进像素

// 稳定窗口类名（单实例 FindWindow 用，保证每次运行类名一致可被找到）
const wchar_t* WNDCLASS_NAME = L"LiteReaderWndClass_v1";

// ----------------------------------------------------------------------------
// 全局状态
// 说明：整个程序采用“全局视图状态”模型——当前激活标签的文本/光标/滚动等信息
//       都存放在下面这组全局变量里。切换标签时通过 snapshotTo / restoreFrom
//       在“全局变量”与“每个文档的快照 Doc”之间互相拷贝。
// ----------------------------------------------------------------------------
HINSTANCE g_hInst = NULL; // 当前模块实例句柄
HWND g_hwnd = NULL;       // 主窗口句柄
HWND g_hFind = NULL;      // 查找输入框（NULL 表示未显示查找条）
// 查找条上的“上一项/下一项/关闭”改为“自绘区域”（非子控件），以下为它们的命中矩形与交互状态
const int FIND_H = 30;                             // 查找条高度（像素）
RECT g_rPrev = { 0 }, g_rNext = { 0 }, g_rClose = { 0 }; // 三个按钮的命中矩形
int g_findHover = 0; // 当前鼠标悬停的按钮：0=无 1=上一项 2=下一项 3=关闭
int g_findPress = 0; // 当前按下的按钮（同上枚举），用于按下态绘制

// 自绘顶部菜单栏（替代系统菜单条，以解决 Windows 10 经典菜单条无法随主题变色的问题）
HMENU g_hMenuFile = NULL; // 文件弹出菜单
HMENU g_hMenuEdit = NULL; // 编辑弹出菜单
HMENU g_hMenuView = NULL; // 视图弹出菜单
int g_menuHover = -1;     // 当前悬停的菜单项索引：-1=无
int g_menuActive = -1;    // 当前点下/展开的菜单项索引
RECT g_menuRects[3];      // 三个菜单项的命中矩形

// 双击分词高亮：g_markWord 为当前标记词，g_markFlag 逐字符标记命中，g_markRanges 为所有命中区间
std::wstring g_markWord;
std::vector<char> g_markFlag;                  // 长度等于 g_text.size()，命中字符为 1
std::vector<std::pair<int, int>> g_markRanges; // 所有“整词”命中的 [start,end)

std::wstring g_text;           // 整个文件内容（统一转换为 UTF-16 宽字符串）
std::vector<int> g_lineStart;  // 每一行起始字符在整个 g_text 中的偏移
std::vector<int> g_lineDepth;  // 每一行起始处的括号嵌套深度（用于彩虹括号续行着色）
std::vector<bool> g_lineInBC;  // 每一行起始是否处于块注释 /* */ 中（跨行状态）
std::vector<bool> g_lineInSrv; // 每一行起始是否处于 ASPX 服务端代码块 <% %> 中
std::vector<int>
g_lineInBlock; // 每一行起始是否处于 <script>(1)/<style>(2) 子语言块中（混合语言着色）
std::vector<wchar_t> g_lineBsQ; // 每一行起始处的块字符串引号（python 的三引号 """）
std::vector<int> g_lineLen;     // 每一行长度（字符数，不含换行符）
int g_lineCount = 0;            // 总行数
std::vector<int> g_lineW;       // 每一行像素宽缓存（避免每次交互全文件重算行宽）
int g_maxLineW = 0;             // 全局最大行宽（用于水平滚动范围，缓存）

// 语法 token 缓存：每行解析出来的着色片段
enum TokType {
    T_TEXT = 0,
    T_KEYWORD,
    T_TYPE,
    T_STRING,
    T_COMMENT,
    T_NUMBER,
    T_TAG,
    T_ATTR,
    T_AVAL,
    T_IDENT,
    T_PUNCT,
    T_BRACKET,
    T_FUNC,
    T_PROC,
    T_PREPROC
};
struct Token {
    int start;
    int len;
    unsigned char type;
    unsigned char col;
}; // col 用于括号彩虹色索引
std::unordered_map<int, std::vector<Token>>
g_tokens;                   // 每行 token 懒缓存（仅存已解析行，避免大文件预分配海量空 vector）
std::vector<Token> g_emptyToks; // 空 token 列表（纯文本模式占位，避免每行临时构造）
// 跨行着色状态（块注释/括号深度/ASPX 服务端块/块字符串引号）逐行起始状态：
// 改为“按需计算 + 缓存”，载入时不再全文件扫描；g_stateDone[l] 标记该行起始状态是否已就绪。
std::vector<char> g_stateDone; // 各行起始状态是否已计算（0=未算，1=已算）

// 视图（全局共享，跨标签保持一致）
bool g_dark = true;  // 当前主题的深浅标志（随主题切换自动同步，供 fileAccent 等两态逻辑使用）
int g_themeIdx = 0;  // 当前主题索引（见下方 g_themes 主题数组）
bool g_wrap = false; // 是否自动换行
int g_fontSize = 14; // 字号
// 配置（.ini 持久化）：启动布局与用户偏好
int g_cfgX = 0, g_cfgY = 0, g_cfgW = 0, g_cfgH = 0, g_cfgMax = 0, g_cfgSidebar = 0;
std::wstring g_cfgFolder;
extern std::wstring g_folderPath;   // 根文件夹路径（定义见下方，供 loadConfig/saveConfig 引用）
extern int g_sidebarW;              // 侧栏宽度（定义见下方）
HFONT g_hFont = NULL;               // 当前字体句柄（主编辑框，随 Ctrl+滚轮 变化）
HFONT g_sideFont = NULL;            // 侧栏（文件夹列表）专用字体，固定字号，不随编辑器字号变化
HBITMAP g_caretBmp = NULL;          // 光标纯色位图，随主题（TH.caret）惰性创建，切换主题时销毁重建
int g_charW = 8, g_lineH = 20;      // 单个字符像素宽、单行像素高（measureFont 时测算）
int g_gutterW = 56;                 // 左侧行号区宽度
int g_topLine = 0;                  // 第一条可见“视觉行”的索引（垂直滚动位置）
int g_scrollX = 0;                  // 水平滚动像素
int g_caretOff = 0;                 // 光标字符偏移（相对 g_text 起点）
int g_anchorOff = 0;                // 选区锚点（按住鼠标拖动时的起点）
int g_selStart = -1, g_selEnd = -1; // 选中区间 [selStart, selEnd)（-1 表示无选区）
int g_matchA = -1, g_matchB = -1;   // 括号配对高亮的起始字符偏移（单字符括号或 BEGIN/END 词首）
int g_matchAw = 1, g_matchBw = 1;   // 配对区间长度（字符括号=1，BEGIN/END=5）

// 视觉行：因“自动换行”，一个逻辑行可能拆成多个视觉行。
// Visual 记录该视觉行属于哪个逻辑行(line)、起始列(col)、长度(len)。
struct Visual {
    int line;
    int col;
    int len;
};
std::vector<Visual> g_visual;
int g_visualCount = 0;

// 行渲染缓存：把每个可见视觉行（非选中状态）的文本与语法着色一次性渲染到离屏位图，
// 后续帧（尤其是拖选/滚动）直接 BitBlt，避免对全屏可见行反复逐段 ExtTextOut（着色卡顿主因）。
struct LineBmp {
    HDC dc;
    HBITMAP bmp;
    int w;
    int ver;
    bool cached;
};
std::map<int, LineBmp> g_lineBmp;    // key = 视觉行号 v
int g_renderVer = 0;                 // 内容/字体/主题/换行变化时自增，使所有缓存失效
HDC g_hdcScreen = NULL;              // 兼容 DC 参照（首次使用时创建）
const int LINE_BMP_CAP = 4096;       // 单行位图最大像素宽（超长行不缓存，回退直绘），限制内存
const int LINE_BMP_MAX = 192;        // 缓存行数上限（约等于可见行数），超出淘汰最旧一行
void invalidateLineCache();          // 释放全部行缓存并使版本号失效
void renderLineBase(HDC hdc, int v); // 把视觉行 v 的非选中文本+语法着色渲染到 hdc 的 (0,0)
void drawLineDirect(HDC hdc, int v, int x, int y, int clipR);  // 超长行回退：原始逐段绘制
void drawLineOverlay(HDC hdc, int v, int x, int y, int clipR); // 选中/匹配/标记覆盖层
bool lineNeedsOverlay(int base, int len);                      // 该行是否落在选中/匹配/标记范围内
LineBmp& getLineBmp(int v); // 取（或渲染并缓存）视觉行 v 的离屏位图

std::wstring g_filePath;       // 当前文档的磁盘路径
std::wstring g_lang = L"auto"; // 语言选择（auto / txt / csharp / ...）
int g_enc = 0;        // 当前文档编码：0=UTF-8无BOM 1=UTF-8 BOM 2=UTF-16LE 3=UTF-16BE 4=ANSI(GBK)
bool g_dirty = false; // 文档是否已修改（未保存），标题追加 “ *” 提示

// 代码补全（关键字 / 括号配对 / 函数补全）。默认关闭，由 .ini 的 autocomplete 开关控制。
bool g_autocomplete = false;           // 是否开启代码补全（.ini 持久化，默认关闭）
HWND g_hComp = NULL;                   // 补全候选列表弹出窗口（懒创建）
bool g_compVisible = false;            // 候选列表是否正在显示
std::vector<std::wstring> g_compItems; // 当前候选文本
std::vector<int> g_compKind;           // 候选类型：0=关键字/类型 1=函数
int g_compSel = 0;                     // 高亮项索引
int g_compWordStart = 0;               // 正在补全的词首偏移（替换起点）
int g_compWordEnd = 0;                 // 正在补全的词尾偏移（替换终点）
// 文档内函数名缓存（供“函数补全”使用；文本变化时置脏，下次补全时重建）
std::vector<std::wstring> g_funcNames;
bool g_funcDirty = true;
const int COMP_ITEM_H = 22;      // 候选列表每项高度（像素）
const int COMP_MAX_VISIBLE = 12; // 候选列表最多同时显示项数
const wchar_t* COMP_CLASS = L"LiteReaderCompWnd_v1";
static bool g_compClassRegistered = false;

// 代码补全相关前向声明（实现见文件后部的“代码补全”段落）
void hideCompletion();
void updateCompletion();
void rebuildFuncNames();
void acceptCompletion();
void showCompletion();
static bool isPairOpen(wchar_t);
static bool isPairClose(wchar_t);
static wchar_t pairClose(wchar_t);
static LRESULT CALLBACK CompWndProc(HWND, UINT, WPARAM, LPARAM);
static void registerCompClass();

// 语言枚举
enum Lang {
    L_AUTO,
    L_TXT,
    L_CS,
    L_SQL,
    L_HTML,
    L_JS,
    L_JSON,
    L_PY,
    L_CSS,
    L_XML,
    L_C,
    L_CPP,
    L_JAVA,
    L_ASPX
};
Lang g_langId = L_AUTO; // 实际生效的语言 id

// 关键字集合（不同语言分开存）
std::set<std::wstring> KW_CS, KW_SQL, KW_JS, KW_PY, KW_CSS, TY_CS, KW_C, KW_CPP, KW_JAVA, TY_C,
TY_CPP, TY_JAVA;
// 前缀 KW_ 为关键字，TY_ 为类型/内置类型名（着色为 T_TYPE）

// ----------------------------------------------------------------------------
// 多文档（标签）
// ----------------------------------------------------------------------------
// 撤销/重做：操作式历史。每个 EditStep 记录“被替换区间起点、删除串、插入串”，
// 撤销=删除插入串并重填删除串；重做=反之。比“整篇快照”省内存，适合大文件逐字编辑。
struct EditStep {
    int start;
    std::wstring del;
    std::wstring ins;
};

struct Doc {
    std::wstring text; // 该文档的文本内容
    std::vector<int> lineStart, lineLen, lineDepth, lineW;
    int maxLineW = 0;
    std::vector<bool> lineInBC; // 注意：结构体里用的是 lineInBC，全局里也叫 lineInBC
    std::vector<bool> lineInSrv;
    std::vector<wchar_t> lineBsQ;
    std::unordered_map<int, std::vector<Token>>
        tokens; // 每行 token 懒缓存（切换标签时随快照迁移，仅含已解析行）
    int lineCount = 0;
    std::wstring filePath, lang;
    Lang langId = L_AUTO;
    int caretOff = 0, anchorOff = 0, selStart = -1, selEnd = -1, matchA = -1, matchB = -1,
        matchAw = 1, matchBw = 1;
    int topLine = 0, scrollX = 0;
    std::vector<Visual> visual;
    int visualCount = 0;
    bool dirty = false; // 该文档是否已修改（未保存）
    std::wstring title; // 预留标题字段（当前用 filePath 派生，未单独使用）
    // 撤销/重做栈（操作式历史），按文档保存，切换标签时随快照迁移
    std::vector<EditStep> undoStack;
    std::vector<EditStep> redoStack;
};
std::vector<Doc> g_docs; // 所有打开的文档
int g_active = -1;       // 当前激活标签索引

// 撤销/重做栈（操作式历史，定义见上方 EditStep）。随 snapshotTo/restoreFrom 在文档间迁移。
std::vector<EditStep> g_undoStack; // 当前激活文档的撤销栈
std::vector<EditStep> g_redoStack; // 当前激活文档的重做栈

// 把当前“全局视图状态”快照保存到文档 i（切换标签前先调用，避免丢失当前页状态）
void snapshotTo(int i) {
    if (i < 0 || i >= (int)g_docs.size()) return;
    Doc& d = g_docs[i];
    d.text = g_text;
    d.lineStart = g_lineStart;
    d.lineLen = g_lineLen;
    d.lineDepth = g_lineDepth;
    d.lineInBC = g_lineInBC;
    d.lineInSrv = g_lineInSrv;
    d.lineBsQ = g_lineBsQ;
    d.tokens = g_tokens;
    d.lineW = g_lineW;
    d.maxLineW = g_maxLineW;
    d.lineCount = g_lineCount;
    d.filePath = g_filePath;
    d.lang = g_lang;
    d.langId = g_langId;
    d.caretOff = g_caretOff;
    d.anchorOff = g_anchorOff;
    d.selStart = g_selStart;
    d.selEnd = g_selEnd;
    d.matchA = g_matchA;
    d.matchB = g_matchB;
    d.matchAw = g_matchAw;
    d.matchBw = g_matchBw;
    d.topLine = g_topLine;
    d.scrollX = g_scrollX;
    d.dirty = g_dirty;
    d.visual = g_visual;
    d.visualCount = g_visualCount;
    d.undoStack = g_undoStack;
    d.redoStack = g_redoStack;
}
// 从文档 i 恢复“全局视图状态”（切换标签进来时调用）
void restoreFrom(int i) {
    if (i < 0 || i >= (int)g_docs.size()) return;
    const Doc& d = g_docs[i];
    g_text = d.text;
    g_lineStart = d.lineStart;
    g_lineLen = d.lineLen;
    g_lineDepth = d.lineDepth;
    g_lineInBC = d.lineInBC;
    g_lineInSrv = d.lineInSrv;
    g_lineBsQ = d.lineBsQ;
    g_tokens = d.tokens;
    g_lineW = d.lineW;
    g_maxLineW = d.maxLineW;
    g_lineCount = d.lineCount;
    g_filePath = d.filePath;
    g_lang = d.lang;
    g_langId = d.langId;
    g_caretOff = d.caretOff;
    g_anchorOff = d.anchorOff;
    g_selStart = d.selStart;
    g_selEnd = d.selEnd;
    g_matchA = d.matchA;
    g_matchB = d.matchB;
    g_matchAw = d.matchAw;
    g_matchBw = d.matchBw;
    g_topLine = d.topLine;
    g_scrollX = d.scrollX;
    g_dirty = d.dirty;
    g_visual = d.visual;
    g_visualCount = d.visualCount;
    g_undoStack = d.undoStack;
    g_redoStack = d.redoStack;
    invalidateLineCache(); // 切换标签后内容已变，行缓存失效
    g_stateDone.assign(g_lineCount, 0);
    g_stateDone[0] = 1; // 恢复后跨行状态重新按需计算（仅第0行起始状态必为初始）
    g_funcDirty = true;
    hideCompletion(); // 切换文档：函数名缓存失效、收起补全列表
}

// ----------------------------------------------------------------------------
// 主题 / 颜色
// ----------------------------------------------------------------------------
// 注意 COLORREF 内存布局为 0x00BBGGRR（B 在低位，R 在高位）。
// 例如 0x00DD78C6：RR=DD GG=78 BB=C6，对应 #c678dd。
// 每套主题自带完整配色（语法调色板 + 界面元素），切换主题即整体换肤。
enum { NTHEMES = 5 };
struct Theme {
    const wchar_t* name;
    bool dark;                 // 浅色/深色（驱动 fileAccent 等两态逻辑）
    COLORREF c[T_PREPROC + 1]; // 语法调色板：TEXT..PREPROC
    COLORREF rb[6];            // 彩虹括号（按嵌套深度取色）
    COLORREF bg, gutterBg, gutterFg, selBg, matchBg, markBg, caret, selText, gutterLine;
    COLORREF sbBg, sbHeaderBg, sbText, sbBtnText, sbBtnTextHover, sbFolderCol, sbActiveBg, sbHoverBg,
        sbScrollbar, sbDivider, sbEmptyText;
    COLORREF tabStripBg, tabDivider, tabBg, tabBgActive, tabAccent, tabText, tabTextActive, plusBg,
        plusText;
    COLORREF findBg, findDivider, findIcon, findBtnBg, findBtnBgHover, findBtnBgPress, findBtnText,
        findCloseBg, findCloseHover;
    COLORREF border, caption, captionText; // 窗口边框 / 标题栏 / 标题文字颜色（随主题）
    COLORREF menuBarBg, menuBarDivider, menuBarText, menuBarTextHover, menuBarHover;
};
Theme g_themes[NTHEMES] = {
    // 0 One Dark Pro（原默认深色）
    {L"One Dark Pro",
     true,
     {0x00BFB2AB, 0x00DD78C6, 0x007BC0E5, 0x0079C398, 0x0070635C, 0x00669AD1, 0x00756CE0,
      0x00669AD1, 0x0079C398, 0x00BFB2AB, 0x00968A82, 0x00000000, 0x00EFAF61, 0x00669AD1,
      0x00C2B656},
     {0x00756CE0, 0x007BC0E5, 0x0079C398, 0x00C2B656, 0x00EFAF61, 0x00DD78C6},
     0x00342C28,
     0x002B2521,
     0x0070635C,
     0x0051443E,
     0x00525220,
     0x0046401A,
     0x00FFFFFF,
     0x00FFFFFF,
     0x00333333,
     0x002B2521,
     0x001F1B18,
     0x009DA5B4,
     0x00888888,
     0x00FFFFFF,
     0x007BC0E5,
     0x00714709,
     0x002A2D2E,
     0x00545454,
     0x00000000,
     0x00666666,
     0x00252526,
     0x003A3A3A,
     0x002D2D2D,
     0x001E1E1E,
     0x00569CD6,
     0x00C0C0C0,
     0x00FFFFFF,
     0x002D2D2D,
     0x00FFFFFF,
     0x002B2521,
     0x003A3A3A,
     0x009DA5B4,
     0x003A6EA5,
     0x004A82BE,
     0x002A5278,
     0x00FFFFFF,
     0x003A3A3A,
     0x008A3B3B,
     0x00252526,
     0x00252526,
     0x00D4D4D4,
     0x00252526,
     0x003A3A3A,
     0x00D4D4D4,
     0x00FFFFFF,
     0x003A3A3A},
     // 1 One Light（原默认浅色）
     {L"One Light",
      false,
      {0x002E2924, 0x00A426A6, 0x00016898, 0x004AA150, 0x00998F8B, 0x00164BCB, 0x005B18C2,
       0x00016898, 0x004AA150, 0x002E2924, 0x006A6057, 0x00000000, 0x00F27840, 0x00164BCB,
       0x00BC8401},
      {0x003439C2, 0x000089B5, 0x003B8A2F, 0x00A3851A, 0x00BF6F1F, 0x00A34192},
      0x00FAFAFA,
      0x00F0F0F0,
      0x00999999,
      0x00FFE8CF,
      0x00A0F3FF,
      0x00BFE6C0,
      0x00000000,
      0x00000000,
      0x00E1E4E8,
      0x00F3F3F3,
      0x00ECECEC,
      0x00444444,
      0x00999999,
      0x00000000,
      0x002D7DD2,
      0x00D2E7FF,
      0x00E6E6E6,
      0x00BFBFBF,
      0x00DADADA,
      0x00999999,
      0x00F0F0F0,
      0x00D0D0D0,
      0x00E4E4E4,
      0x00FFFFFF,
      0x001976D2,
      0x00555555,
      0x00000000,
      0x00E4E4E4,
      0x00000000,
      0x00F3F3F3,
      0x00D0D0D0,
      0x00555555,
      0x002F7FD1,
      0x004A95DD,
      0x001F5FA0,
      0x00FFFFFF,
      0x00D0D0D0,
      0x00C0392B,
      0x00D0D0D0,
      0x00F3F3F3,
      0x00000000,
      0x00F3F3F3,
      0x00D0D0D0,
      0x00000000,
      0x00000000,
      0x00E4E4E4},
      // 2 VS Code Dark+（仿 VS Code 原生深色）
      {L"VS Code",
       true,
       {0x00D4D4D4, 0x00D69C56, 0x00B0C94E, 0x007891CE, 0x0055996A, 0x00A8CEB5, 0x00D69C56,
        0x00FEDC9C, 0x007891CE, 0x00D4D4D4, 0x00D4D4D4, 0x00000000, 0x00AADCDC, 0x00AADCDC,
        0x00C0C686},
       {0x00AAE0FF, 0x00D69C56, 0x00C0C686, 0x007891CE, 0x00B0C94E, 0x00D19A66},
       0x001E1E1E,
       0x001E1E1E,
       0x00858585,
       0x00784F26,
       0x003D3737,
       0x0032323A,
       0x00FFFFFF,
       0x00FFFFFF,
       0x00202020,
       0x00262526,
       0x00333333,
       0x00CCCCCC,
       0x00999999,
       0x00FFFFFF,
       0x00C5C5C5,
       0x003D3737,
       0x002A2D2E,
       0x00424242,
       0x00202020,
       0x00888888,
       0x00252526,
       0x00202020,
       0x002D2D2D,
       0x001E1E1E,
       0x00CC7A00,
       0x00969696,
       0x00FFFFFF,
       0x002D2D2D,
       0x00FFFFFF,
       0x00262526,
       0x00202020,
       0x00CCCCCC,
       0x003A6EA5,
       0x004A82BE,
       0x002A5278,
       0x00FFFFFF,
       0x00333333,
       0x008A3B3B,
       0x00252526,
       0x00333333,
       0x00D4D4D4,
       0x00252526,
       0x00202020,
       0x00CCCCCC,
       0x00FFFFFF,
       0x00333333},
       // 3 IntelliJ IDEA（仿 IDEA 原生 Darcula 深色）
       {L"IntelliJ IDEA",
        true,
        {0x00C6B7A9, 0x003278CC, 0x006DC6FF, 0x0059876A, 0x00808080, 0x00BB9768, 0x003278CC,
         0x00BABABA, 0x0059876A, 0x00C6B7A9, 0x00C6B7A9, 0x00000000, 0x006DC6FF, 0x006DC6FF,
         0x003278CC},
        {0x00C6B7A9, 0x003278CC, 0x00BB9768, 0x0059876A, 0x006DC6FF, 0x00999999},
        0x002B2B2B,
        0x00353331,
        0x00909090,
        0x00834221,
        0x00714909,
        0x004A4946,
        0x00FFFFFF,
        0x00FFFFFF,
        0x00353536,
        0x00353331,
        0x00353331,
        0x00A9B7C6,
        0x00888888,
        0x00FFFFFF,
        0x00A9B7C6,
        0x00302F2D,
        0x0036383A,
        0x005A5D5E,
        0x00262627,
        0x00888888,
        0x00413F3C,
        0x00262627,
        0x0047474A,
        0x002B2B2B,
        0x003278CC,
        0x00A8B0BC,
        0x00FFFFFF,
        0x0047474A,
        0x00FFFFFF,
        0x00353331,
        0x00262627,
        0x00A9B7C6,
        0x003A6EA5,
        0x004A82BE,
        0x002A5278,
        0x00FFFFFF,
        0x00333333,
        0x008A3B3B,
        0x0047474A,
        0x0047474A,
        0x00A8B0BC,
        0x0047474A,
        0x00262627,
        0x00A8B0BC,
        0x00FFFFFF,
        0x0036383A},
        // 4 极致黑（纯黑背景高对比）
        {L"极致黑",
         true,
         {0x00FFFFFF, 0x00FFC14F, 0x00B0C94E, 0x007891CE, 0x0055996A, 0x00A8CEB5, 0x00FFC14F,
          0x00FEDC9C, 0x007891CE, 0x00FFFFFF, 0x00E0E0E0, 0x00000000, 0x00AADCDC, 0x00AADCDC,
          0x00C0C686},
         {0x00FFC14F, 0x00FEDC9C, 0x00C0C686, 0x007891CE, 0x00B0C94E, 0x00AADCDC},
         0x00000000,
         0x00050505,
         0x00666666,
         0x00784F26,
         0x003A3A3A,
         0x002A2A2A,
         0x00FFFFFF,
         0x00FFFFFF,
         0x00222222,
         0x00050505,
         0x000A0A0A,
         0x00CCCCCC,
         0x00888888,
         0x00FFFFFF,
         0x00FFC080,
         0x00141414,
         0x00101010,
         0x00333333,
         0x00181818,
         0x00888888,
         0x00000000,
         0x00222222,
         0x00141414,
         0x00000000,
         0x00FFC14F,
         0x00CCCCCC,
         0x00FFFFFF,
         0x00141414,
         0x00FFFFFF,
         0x00050505,
         0x00181818,
         0x00CCCCCC,
         0x003A6EA5,
         0x004A82BE,
         0x002A5278,
         0x00FFFFFF,
         0x00181818,
         0x008A3B3B,
         0x00222222,
         0x00050505,
         0x00FFFFFF,
         0x00050505,
         0x00222222,
         0x00CCCCCC,
         0x00FFFFFF,
         0x00141414},
};
#define TH (g_themes[g_themeIdx])

// 以下为各界面元素的背景/前景色（依据当前主题返回）
COLORREF bgColor() {
    return TH.bg;
}
COLORREF gutterBg() {
    return TH.gutterBg;
}
COLORREF gutterFg() {
    return TH.gutterFg;
}
COLORREF selBg() {
    return TH.selBg;
}
COLORREF matchBg() {
    return TH.matchBg;
}
COLORREF markBg() {
    return TH.markBg;
}

// ----------------------------------------------------------------------------
// 配置（LiteReader.ini 持久化：主题 / 字号 / 启动布局）
// ----------------------------------------------------------------------------
std::wstring iniPath() {
    wchar_t buf[MAX_PATH];
    GetModuleFileName(NULL, buf, MAX_PATH);
    std::wstring p(buf);
    size_t k = p.find_last_of(L'\\');
    if (k != std::wstring::npos) p = p.substr(0, k + 1);
    return p + L"LiteReader.ini";
}
void loadConfig() {
    std::wstring ip = iniPath();
    int t = GetPrivateProfileInt(L"Settings", L"theme", 0, ip.c_str());
    if (t < 0 || t >= NTHEMES) t = 0;
    g_themeIdx = t;
    g_dark = g_themes[t].dark;
    int fs = GetPrivateProfileInt(L"Settings", L"fontsize", 14, ip.c_str());
    if (fs < 9) fs = 9;
    if (fs > 28) fs = 28;
    g_fontSize = fs;
    g_cfgX = GetPrivateProfileInt(L"Settings", L"x", 0, ip.c_str());
    g_cfgY = GetPrivateProfileInt(L"Settings", L"y", 0, ip.c_str());
    g_cfgW = GetPrivateProfileInt(L"Settings", L"w", 0, ip.c_str());
    g_cfgH = GetPrivateProfileInt(L"Settings", L"h", 0, ip.c_str());
    g_cfgMax = GetPrivateProfileInt(L"Settings", L"max", 0, ip.c_str());
    int sb = GetPrivateProfileInt(L"Settings", L"sidebar", 0, ip.c_str());
    if (sb >= SIDEBAR_W_MIN && sb <= SIDEBAR_W_MAX) g_sidebarW = sb;
    wchar_t fb[MAX_PATH];
    if (GetPrivateProfileString(L"Settings", L"folder", L"", fb, MAX_PATH, ip.c_str()))
        g_cfgFolder = fb;
    int ac = GetPrivateProfileInt(L"Settings", L"autocomplete", 0, ip.c_str());
    g_autocomplete = (ac != 0);
}
void saveConfig() {
    std::wstring ip = iniPath();
    g_cfgFolder = g_folderPath; // 始终持久化当前侧栏文件夹
    if (g_hwnd) {
        WINDOWPLACEMENT wp = { sizeof(wp) };
        if (GetWindowPlacement(g_hwnd, &wp)) {
            RECT r = wp.rcNormalPosition;
            g_cfgX = r.left;
            g_cfgY = r.top;
            g_cfgW = r.right - r.left;
            g_cfgH = r.bottom - r.top;
            g_cfgMax = (wp.showCmd == SW_SHOWMAXIMIZED || wp.showCmd == SW_MAXIMIZE) ? 1 : 0;
        }
    }
    auto W = [&](const wchar_t* k, int v) {
        std::wstring s = std::to_wstring(v);
        WritePrivateProfileString(L"Settings", k, s.c_str(), ip.c_str());
        };
    W(L"theme", g_themeIdx);
    W(L"fontsize", g_fontSize);
    W(L"autocomplete", g_autocomplete ? 1 : 0);
    W(L"x", g_cfgX);
    W(L"y", g_cfgY);
    W(L"w", g_cfgW);
    W(L"h", g_cfgH);
    W(L"max", g_cfgMax);
    if (g_sidebarW >= SIDEBAR_W_MIN && g_sidebarW <= SIDEBAR_W_MAX) W(L"sidebar", g_sidebarW);
    WritePrivateProfileString(L"Settings", L"folder", g_cfgFolder.empty() ? L"" : g_cfgFolder.c_str(),
        ip.c_str());
    WritePrivateProfileString(L"Settings", L"version", L"1", ip.c_str());
}
void applyThemeToFrame(); // 前向声明（定义见下方）
void setTheme(int i) {
    if (i < 0 || i >= NTHEMES) i = 0;
    g_themeIdx = i;
    g_dark = g_themes[i].dark;
    if (g_caretBmp) {
        DeleteObject(g_caretBmp);
        g_caretBmp = NULL;
    } // 切换主题后重建光标位图
    invalidateLineCache();
    if (g_hwnd) {
        InvalidateRect(g_hwnd, NULL, TRUE);
        applyThemeToFrame();
    }
}
// 让窗口边框/标题栏/标题文字颜色随当前主题变化（DWM 属性，需 Windows 11；旧系统静默忽略）
#ifndef DWMWA_BORDER_COLOR
#define DWMWA_BORDER_COLOR 34
#endif
#ifndef DWMWA_CAPTION_COLOR
#define DWMWA_CAPTION_COLOR 35
#endif
#ifndef DWMWA_TEXT_COLOR
#define DWMWA_TEXT_COLOR 36
#endif
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20 // Win11 SDK 编号；Win10(20H1+) 用 19，下方会回退
#endif
void applyThemeToFrame() {
    if (!g_hwnd) return;
    HMODULE h = LoadLibraryW(L"dwmapi.dll");
    if (!h) return;
    typedef HRESULT(WINAPI* DswSetAttr)(HWND, DWORD, const void*, DWORD);
    DswSetAttr fn = (DswSetAttr)GetProcAddress(h, "DwmSetWindowAttribute");
    if (fn) {
        COLORREF b = TH.border, c = TH.caption, t = TH.captionText;
        fn(g_hwnd, DWMWA_BORDER_COLOR, &b, sizeof(b));  // 窗口边框（1px 外框，Win11）
        fn(g_hwnd, DWMWA_CAPTION_COLOR, &c, sizeof(c)); // 标题栏背景（Win11）
        fn(g_hwnd, DWMWA_TEXT_COLOR, &t, sizeof(t));    // 标题文字（Win11）
        // 沉浸式深色模式：让整个非客户区（标题栏、菜单栏、边框）随主题深浅切换。
        // 这是 Win10(20H1+) 与 Win11 都能稳定着色【菜单栏】与【边框】的唯一可靠方式 —
        // 34/35/36 仅 Win11 且仅覆盖标题栏与边框，不包含菜单栏。
        BOOL dm = TH.dark ? TRUE : FALSE;
        if (fn(g_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dm, sizeof(dm)) != S_OK) {
            DWORD a19 = 19; // 旧系统（Win10 早期）该属性编号为 19
            fn(g_hwnd, a19, &dm, sizeof(dm));
        }
        // 强制重绘非客户区，使新边框/标题栏/菜单栏色立即生效
        RedrawWindow(g_hwnd, NULL, NULL, RDW_FRAME | RDW_INVALIDATE | RDW_UPDATENOW);
    }
    FreeLibrary(h);
}

// ----------------------------------------------------------------------------
// 工具函数
// ----------------------------------------------------------------------------
inline int min3(int a, int b, int c) {
    int m = a;
    if (b < m) m = b;
    if (c < m) m = c;
    return m;
} // 三数取小（预留，当前未大量使用）

// 前向声明：在 scanLine / paint 等函数定义之前，先把会调用到的函数声明出来
void updateScroll();
std::wstring selectedOrWordAtCaret(); // 取目标标识符（选区首词 / 光标整词），供跳转定义菜单使用
void gotoDefinition(const std::wstring& name); // 在文档内按启发式跳转到标识符定义
int leftBar();                                 // 左侧栏宽度（未打开文件夹时为 0）
void updateCaretPos();
void setWindowTitle();
std::wstring openFileDialog();
void openInNewTab(const std::wstring& path);
void switchTab(int j);
void closeTab(int i);
void createMenus();                                     // 创建顶部自绘菜单栏使用的弹出菜单
void drawMenuBar(HDC mem, const RECT& rc);              // 自绘菜单栏（文件/编辑/视图）
int menuBarHit(int x, const RECT& rc);                  // 返回 0=文件 1=编辑 2=视图，-1=无
void showMenuPopup(int idx, HWND hwnd, const RECT& rc); // 展开某一项弹出菜单

// 二分查找：给定字符偏移 off，返回它属于第几行
// g_lineStart 是单调递增的，所以用二分；结果 res 为最后一个 <= off 的行。
int lineOfOffset(int off) {
    int lo = 0, hi = g_lineCount - 1, res = 0;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (g_lineStart[mid] <= off) {
            res = mid;
            lo = mid + 1;
        }
        else
            hi = mid - 1;
    }
    return res;
}
// 给定行与偏移，返回该偏移在行内的列号（偏移 - 行首偏移）
int colOfOffset(int line, int off) {
    return off - g_lineStart[line];
}

// ---- 字符宽度（支持中文/全角：在等宽字体下占 2 格） ----
// 说明：程序用 Consolas 等宽字体，但等宽字体下 ASCII 占 1 格、CJK/全角占 2 格，
//       因此需要单独判断“宽字符”，才能正确计算像素位置。
inline bool isWideChar(wchar_t c) {
    if (c == 0) return false;
    if (c >= 0x1100 && c <= 0x115F) return true;   // 谚文兼容字母
    if (c >= 0x2E80 && c <= 0x303E) return true;   // CJK 部首/符号/标点
    if (c >= 0x3041 && c <= 0x33FF) return true;   // 平假名/片假名/CJK 符号
    if (c >= 0x3400 && c <= 0x4DBF) return true;   // CJK 扩展A
    if (c >= 0x4E00 && c <= 0x9FFF) return true;   // CJK 统一表意文字（常用汉字）
    if (c >= 0xA000 && c <= 0xA4CF) return true;   // 彝文
    if (c >= 0xAC00 && c <= 0xD7A3) return true;   // 谚文音节
    if (c >= 0xF900 && c <= 0xFAFF) return true;   // 兼容表意
    if (c >= 0xFE30 && c <= 0xFE4F) return true;   // CJK 兼容形式
    if (c >= 0xFF00 && c <= 0xFFEF) return true;   // 全角 ASCII / 半角片假名
    if (c >= 0x20000 && c <= 0x2FA1F) return true; // CJK 扩展B+
    return false;
}
// 返回单个字符的像素宽度（宽字符为字符宽的 2 倍）
inline int charW(wchar_t c) {
    return isWideChar(c) ? g_charW * 2 : g_charW;
}
// PascalCase / 首字母大写的多字符词 —— 在 C#/JS 中通常代表类型/类名/构造函数
inline bool isUpperWord(const std::wstring& w) {
    if (w.size() < 2) return false;
    return iswupper((wchar_t)w[0]) != 0;
}
// 计算某一行 [0,col) 区间的累计像素宽度（用于把列号换算成 x 坐标）
int linePrefixPx(int line, int col) {
    if (col < 0) col = 0;
    int s = g_lineStart[line], n = s + col, w = 0;
    for (int i = s; i < n; i++) w += charW(g_text[i]);
    return w;
}
// 整行像素宽度：优先用缓存 g_lineW（O(1)），缓存未建立时回退遍历
int linePx(int line) {
    if (line >= 0 && line < (int)g_lineW.size() && g_lineW[line] >= 0) return g_lineW[line];
    int s = g_lineStart[line], n = s + g_lineLen[line], w = 0;
    for (int i = s; i < n; i++) w += charW(g_text[i]);
    if (line >= 0 && line < (int)g_lineW.size()) {
        g_lineW[line] = w;
        if (w > g_maxLineW) g_maxLineW = w;
    } // 懒缓存并刷新最大行宽
    return w;
}
// 载入/字号变化时，仅“采样”若干行估算最大行宽，给出初始水平滚动范围；
// 真实逐行像素宽改为按需懒计算（linePx 命中即缓存），不再全文件 O(n) 扫描（大文件卡死主因）。
void seedMaxLineW() {
    int n = g_lineCount;
    if (n == 0) return;
    int step = (n > 200000) ? (n / 200000) : 1; // 约采样 20 万行，足够估算最大行宽
    for (int l = 0; l < n; l += step) (void)linePx(l);
}
// 字符段 [start,start+len) 的像素宽度
int runPx(int start, int len) {
    int w = 0;
    for (int i = start; i < start + len; i++) w += charW(g_text[i]);
    return w;
}
// 从行首按像素偏移反查列号（取字符“中点”为边界，使鼠标点击命中更自然）
int pxToColAbs(int line, int px) {
    // 防御：行号越界（理论上不应发生，但避免任何状态下访问 g_lineStart/g_lineLen 的负/越界下标）
    if (line < 0 || line >= g_lineCount || g_lineStart.empty()) return 0;
    int len = g_lineLen[line], w = 0;
    for (int c = 0; c < len; c++) {
        int cw = charW(g_text[g_lineStart[line] + c]);
        if (w + cw / 2 >= px) return c; // 过了字符中点就算到该字符
        w += cw;
    }
    return len;
}

// 激活已存在窗口：仅当最小化时恢复，否则只置前 —— 不改变大小/位置/最大化状态
void bringToFront(HWND hw) {
    if (IsIconic(hw)) ShowWindow(hw, SW_RESTORE); // 最小化则恢复窗口
    SetForegroundWindow(hw);                      // 否则仅提到最前
}

// ----------------------------------------------------------------------------
// 查找条 / 分词高亮：辅助函数
// ----------------------------------------------------------------------------
// 编辑器可视区顶部 y：显示查找条时整体下移 FIND_H，避免首行被查找条遮挡。
int editorTop() {
    return MENU_H + TAB_H + (g_hFind ? FIND_H : 0);
}

// 判断字符是否构成“单词”（标识符）的一部分：字母/数字/下划线/美元符/Python 的 @
inline bool isWordChar(wchar_t c) {
    return (iswalnum((wchar_t)c) != 0) || c == L'_' || c == L'$' || c == L'@';
}
// 给定光标偏移 off，向左右扩展出完整单词边界，写入 [ws,we)
void wordAtOffset(int off, int& ws, int& we) {
    int len = (int)g_text.size();
    if (off < 0 || off >= len) {
        ws = off;
        we = off;
        return;
    }
    ws = off;
    we = off;
    // 仅当光标落在单词字符上（词中/词首）才向右扩展；若落在单词“之后”
    // （词尾、空白、换行），we 保持为 off，避免跨过非单词字符吞掉下一行/下一个词。
    // 修复：接受补全时把换行符/右括号一起吞掉的问题。
    if (isWordChar(g_text[off])) {
        we = off + 1;
        while (we < len && isWordChar(g_text[we])) we++;
    }
    while (ws > 0 && isWordChar(g_text[ws - 1])) ws--; // 向左扩展
}
// 收集 g_markWord 的所有“整词”命中区间（前后均非单词字符，避免 in 命中 index 这类子串），
// 结果写入 g_markRanges 与逐字符标记 g_markFlag（供 paint 直接查表）。
void collectMarks() {
    g_markRanges.clear();
    int n = (int)g_markWord.size();
    g_markFlag.assign((int)g_text.size(), 0); // 重置为全 0
    if (n == 0) return;
    int len = (int)g_text.size();
    for (int p = 0; p + n <= len;) {
        if (g_text.compare(p, n, g_markWord) == 0) {
            bool okPrev = (p == 0) || !isWordChar(g_text[p - 1]);       // 词首前不能是单词字符
            bool okNext = (p + n >= len) || !isWordChar(g_text[p + n]); // 词尾后不能是单词字符
            if (okPrev && okNext) {
                g_markRanges.push_back({ p, p + n });
                for (int k = p; k < p + n; k++) g_markFlag[k] = 1;
            }
            p += n; // 跳过本词，避免同位置重复
        }
        else
            p++;
    }
}
// 自绘一个圆角按钮（用于查找条的“上一项/下一项”）
void drawFindButton(HDC hdc, RECT r, const wchar_t* text, bool hover, bool pressed) {
    COLORREF base = TH.findBtnBg;
    COLORREF col = pressed ? TH.findBtnBgPress : (hover ? TH.findBtnBgHover : base);
    HBRUSH b = CreateSolidBrush(col);
    HPEN p = CreatePen(PS_SOLID, 1, col); // 边框同色，避免深色描边
    HPEN op = (HPEN)SelectObject(hdc, p);
    HBRUSH ob = (HBRUSH)SelectObject(hdc, b);
    RoundRect(hdc, r.left, r.top, r.right, r.bottom, 6, 6); // 圆角矩形：填充+描边
    SelectObject(hdc, op);
    DeleteObject(p);
    SelectObject(hdc, ob);
    DeleteObject(b);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, TH.findBtnText);
    DrawText(hdc, text, -1, &r, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
}
// 自绘关闭按钮（×）：普通为中性灰，悬停时变红
void drawFindCloseBtn(HDC hdc, RECT r, bool hover, bool pressed) {
    COLORREF col = hover ? TH.findCloseHover : TH.findCloseBg;
    HBRUSH b = CreateSolidBrush(col);
    HPEN p = CreatePen(PS_SOLID, 1, col);
    HPEN op = (HPEN)SelectObject(hdc, p);
    HBRUSH ob = (HBRUSH)SelectObject(hdc, b);
    RoundRect(hdc, r.left, r.top, r.right, r.bottom, 6, 6);
    SelectObject(hdc, op);
    DeleteObject(p);
    SelectObject(hdc, ob);
    DeleteObject(b);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, hover ? RGB(255, 255, 255) : TH.tabText);
    DrawText(hdc, L"×", 1, &r, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
}

// ----------------------------------------------------------------------------
// 语言识别
// 根据 g_lang（用户选择）与 g_filePath（扩展名）推导实际语言枚举。
// g_lang 为 "auto" 时按扩展名推断；否则按用户指定字符串直接映射。
// ----------------------------------------------------------------------------
Lang langFromName() {
    if (g_lang == L"auto") {
        if (g_filePath.empty()) return L_TXT;
        std::wstring ext = g_filePath.substr(g_filePath.find_last_of(L'.') + 1); // 取扩展名
        if (ext == L"cs") return L_CS;
        if (ext == L"sql") return L_SQL;
        if (ext == L"html" || ext == L"htm") return L_HTML;
        if (ext == L"js" || ext == L"mjs" || ext == L"cjs") return L_JS;
        if (ext == L"json") return L_JSON;
        if (ext == L"py" || ext == L"pyw") return L_PY;
        if (ext == L"css") return L_CSS;
        if (ext == L"c" || ext == L"h") return L_C;
        if (ext == L"cpp" || ext == L"cc" || ext == L"cxx" || ext == L"c++" || ext == L"hpp" ||
            ext == L"hxx" || ext == L"hh")
            return L_CPP;
        if (ext == L"java") return L_JAVA;
        if (ext == L"aspx" || ext == L"asax" || ext == L"ascx" || ext == L"ashx" || ext == L"asmx" ||
            ext == L"master")
            return L_ASPX;
        if (ext == L"xml" || ext == L"xaml" || ext == L"svg" || ext == L"config" || ext == L"csproj" ||
            ext == L"vcxproj" || ext == L"resx")
            return L_XML;
        return L_TXT;
    }
    // 用户手动指定了语言
    if (g_lang == L"txt") return L_TXT;
    if (g_lang == L"csharp") return L_CS;
    if (g_lang == L"sql") return L_SQL;
    if (g_lang == L"html") return L_HTML;
    if (g_lang == L"js") return L_JS;
    if (g_lang == L"json") return L_JSON;
    if (g_lang == L"python") return L_PY;
    if (g_lang == L"css") return L_CSS;
    if (g_lang == L"c") return L_C;
    if (g_lang == L"cpp") return L_CPP;
    if (g_lang == L"java") return L_JAVA;
    if (g_lang == L"aspx") return L_ASPX;
    if (g_lang == L"xml") return L_XML;
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
void scanLine(const wchar_t* s, int n, Lang lang, bool& inBC, int& inBlock, bool& inSrv,
    wchar_t& bsQ, int& depth, std::vector<Token>* out) {
    int i = 0;
    bool isClike = (lang == L_CS || lang == L_JS || lang == L_JSON || lang == L_CSS ||
        lang == L_SQL || lang == L_C || lang == L_CPP || lang == L_JAVA);
    bool lineComment = false;
    wchar_t lcChar = 0;
    if (lang == L_CS || lang == L_JS || lang == L_CSS || lang == L_C || lang == L_CPP ||
        lang == L_JAVA)
        lcChar = L'/'; // C 族行注释 //
    else if (lang == L_SQL)
        lcChar = L'-'; // SQL 行注释 --
    else if (lang == L_PY)
        lcChar = L'#';             // Python 行注释 #
    lineComment = (lcChar != 0); // 行注释开关
    bool blockCmt = (lang == L_CS || lang == L_JS || lang == L_CSS || lang == L_SQL || lang == L_C ||
        lang == L_CPP || lang == L_JAVA); // 是否支持 /* */ 块注释
    bool afterProcKw = false;  // 上一个关键字是 EXEC/PROCEDURE 时，下一个标识符即存储过程名
    bool afterProcDot = false; // 过程名被 schema 限定(dbo.x)时，跳过 schema 名，染真正的 proc 名
    // 局部 lambda：若有 out 则把 token 追加进去
    auto add = [&](int st, int len, TokType t, unsigned char col) {
        if (out) out->push_back({ st, len, (unsigned char)t, col });
        };
    while (i < n) {
        wchar_t c = s[i];
        // ---- 行首若已经在块注释中：一直吃到 */ 结束 ----
        if (inBC) {
            int j = i;
            while (j + 1 < n && !(s[j] == L'*' && s[j + 1] == L'/')) j++;
            if (j + 1 < n) {
                add(i, j + 2 - i, T_COMMENT, 0);
                inBC = false;
                i = j + 2;
            } // 找到 */，整段作为注释，离开块注释
            else {
                add(i, n - i, T_COMMENT, 0);
                i = n;
            } // 到行尾都没找到 */，整行是注释
            continue;
        }
        // ---- C/C++/Java 预处理器指令（#include / #define / #pragma ...） ----
        // 条件：行首（或仅空白后）以 # 开头。
        if ((lang == L_C || lang == L_CPP || lang == L_JAVA) && c == L'#') {
            int k2 = 0;
            while (k2 < i && (s[k2] == L' ' || s[k2] == L'\t')) k2++; // 数前导空白
            if (k2 == i) {                                            // # 之前只有空白 => 视为预处理指令
                int j = i + 1;
                while (j < n && (iswalnum(s[j]) || s[j] == L'_')) j++; // 读指令名
                add(i, j - i, T_PREPROC, 0);
                i = j;
                continue;
            }
        }
        // ---- HTML / ASPX / XML 标签与结构 ----
        if (lang == L_HTML || lang == L_ASPX || lang == L_XML) {
            // 混合语言：已在 <script>(JS) 或 <style>(CSS) 子块内，整段按对应子语言着色，
            // 直到遇到对应的结束标签（跨行状态由 inBlock 维护：0=无 1=script/JS 2=style/CSS）。
            if (inBlock != 0) {
                Lang sub = (inBlock == 1) ? L_JS : L_CSS;                            // 子块对应的子语言
                const wchar_t* closeTok = (inBlock == 1) ? L"</script" : L"</style"; // 对应的结束标签前缀
                int cLen = (int)wcslen(closeTok);
                int j = i;
                bool foundClose = false;
                while (j + cLen - 1 < n) {
                    if (wcsncmp(s + j, closeTok, cLen) == 0) {
                        foundClose = true;
                        break;
                    }
                    j++;
                }
                int fragLen = foundClose ? (j - i) : (n - i); // 本行内子语言片段长度
                std::vector<Token> tmp;
                bool x = inBC;
                int ib = 0;
                bool is = false;
                wchar_t z = bsQ;
                int d = depth;
                scanLine(s + i, fragLen, sub, x, ib, is, z, d,
                    &tmp); // 递归用 JS/CSS 规则扫描（子块内不嵌套）
                for (auto& t : tmp) {
                    t.start += i;
                    add(t.start, t.len, (TokType)t.type, t.col);
                }
                inBC = x;
                depth = d;
                bsQ = z; // 回写跨行状态
                if (foundClose) {
                    int k = j + cLen;
                    while (k < n && s[k] != L'>') k++; // 跳过结束标签到 '>'
                    int e = (k < n) ? k + 1 : n;
                    add(j, e - j, T_TAG, 0); // 结束标签整体按标签色
                    inBlock = 0;
                    i = e;
                    continue; // 退出子块
                }
                else {
                    i = n;
                    continue;
                } // 子块跨行，状态保留
            }
            // ASPX 服务端代码块：<% ... %> 内的内容按 C# 着色（且可跨行，状态由 inSrv 维护）。仅
            // HTML/ASPX 生效，XML 不处理。
            if ((lang == L_HTML || lang == L_ASPX) && inSrv) {
                int j = i;
                while (j + 1 < n && !(s[j] == L'%' && s[j + 1] == L'>')) j++; // 找 %>
                int fragLen = (j + 1 < n) ? j - i : n - i;                    // 本行内服务端片段长度
                std::vector<Token> tmp;
                bool x = inBC;
                int ib2 = 0;
                bool srv = false;
                wchar_t z = 0;
                int d = depth;
                scanLine(s + i, fragLen, L_CS, x, ib2, srv, z, d, &tmp); // 递归用 C# 规则扫描这段
                for (auto& t : tmp) {
                    t.start += i;
                    add(t.start, t.len, (TokType)t.type, t.col);
                }
                depth = d;
                if (j + 1 < n) {
                    add(j, 2, T_PUNCT, 0);
                    inSrv = false;
                    inBC = false;
                    i = j + 2;
                } // 遇到 %>，闭合服务端块
                else {
                    i = n;
                } // 服务端块跨行，状态保留
                continue;
            }
            // 进入服务端块：<% 或 <%@ / <%= / <%# / <%$ / <% （仅 HTML/ASPX，XML 的 < 一律按标签处理）
            if ((lang == L_HTML || lang == L_ASPX) && c == L'<' && i + 1 < n && s[i + 1] == L'%') {
                wchar_t d2 = (i + 2 < n) ? s[i + 2] : 0;
                int plen = 2;
                if (d2 == '@' || d2 == '=' || d2 == '#' || d2 == '$' || d2 == ':')
                    plen = 3; // 带修饰符的块多一个字符
                add(i, plen, T_PUNCT, 0);
                inSrv = true;
                i += plen;
                continue;
            }
            // XML 处理指令 <?xml ... ?> 与 <? ... ?>（声明 / PI），用注释色
            if (lang == L_XML && c == L'<' && i + 1 < n && s[i + 1] == L'?') {
                int j = i + 2;
                while (j + 1 < n && !(s[j] == L'?' && s[j + 1] == L'>')) j++;
                int end = (j + 1 < n) ? j + 2 : n;
                add(i, end - i, T_COMMENT, 0);
                i = end;
                continue;
            }
            // XML CDATA 段 <![CDATA[ ... ]]>，整体作为字符串色突出
            if (lang == L_XML && c == L'<' && i + 3 < n && s[i + 1] == L'!' && s[i + 2] == L'[' &&
                s[i + 3] == L'C') {
                int j = i + 4;
                while (j + 2 < n && !(s[j] == L']' && s[j + 1] == L']' && s[j + 2] == L'>')) j++;
                int end = (j + 2 < n) ? j + 3 : n;
                add(i, end - i, T_STRING, 0);
                i = end;
                continue;
            }
            // XML 的 <!DOCTYPE ...> / <!ENTITY ...> 等声明块（排除 <!-- 注释，下面单独处理）
            if (lang == L_XML && c == L'<' && i + 1 < n && s[i + 1] == L'!' &&
                !(i + 3 < n && s[i + 2] == L'-' && s[i + 3] == L'-')) {
                int j = i + 2;
                while (j < n && s[j] != L'>') j++;
                int end = (j < n) ? j + 1 : n;
                add(i, end - i, T_COMMENT, 0);
                i = end;
                continue;
            }
            // XML 实体引用 &name; / &#123;，着色为字符串色
            if (lang == L_XML && c == L'&') {
                int j = i + 1;
                while (j < n && (iswalnum(s[j]) || s[j] == L'#')) j++;
                if (j < n && s[j] == L';') {
                    add(i, j + 1 - i, T_STRING, 0);
                    i = j + 1;
                    continue;
                }
            }
            // HTML 条件注释 <!-- -->
            if (c == L'<' && i + 3 < n && s[i + 1] == L'!' && s[i + 2] == L'-' && s[i + 3] == L'-') {
                int j = i + 4;
                while (j + 2 < n && !(s[j] == L'-' && s[j + 1] == L'-' && s[j + 2] == L'>')) j++;
                int end = (j + 2 < n) ? j + 3 : n;
                add(i, end - i, T_COMMENT, 0);
                i = end;
                continue;
            }
            // 普通标签：<tag attr="val"> 结构着色
            if (c == L'<') {
                int j = i + 1;
                add(i, 1, T_PUNCT, 0); // '<'
                if (j < n && s[j] == L'/') {
                    add(j, 1, T_PUNCT, 0);
                    j++;
                } // 闭合标签的 '/'
                int ns = j;
                while (j < n && (iswalnum(s[j]) || s[j] == L':' || s[j] == L'-')) j++;
                std::wstring tagName;
                if (j > ns) {
                    tagName = std::wstring(s + ns, j - ns);
                    add(ns, j - ns, T_TAG, 0);
                } // 标签名（同时记录供子语言判断）
                while (j < n && s[j] != L'>') {
                    wchar_t ch = s[j];
                    if (ch == L' ' || ch == L'\t' || ch == L'\n' || ch == L'\r') {
                        add(j, 1, T_TEXT, 0);
                        j++;
                        continue;
                    }
                    if (ch == L'=') {
                        add(j, 1, T_PUNCT, 0);
                        j++;
                        continue;
                    }
                    if (ch == L'"' || ch == L'\'') { // 属性值字符串
                        wchar_t q = ch;
                        int k = j + 1;
                        while (k < n && s[k] != q) k++;
                        int e = (k < n) ? k + 1 : k;
                        add(j, e - j, T_AVAL, 0);
                        j = e;
                        continue;
                    }
                    int as = j;
                    while (j < n && (iswalnum(s[j]) || s[j] == L':' || s[j] == L'-' || s[j] == L'.')) j++;
                    if (j > as)
                        add(as, j - as, T_ATTR, 0); // 属性名
                    else {
                        add(j, 1, T_TEXT, 0);
                        j++;
                    }
                }
                if (j < n && s[j] == L'>') {
                    add(j, 1, T_PUNCT, 0);
                    j++;
                } // '>'
                // 进入 <script>/<style> 开始标签后，后续内容按对应子语言着色
                if (i + 1 < n && s[i + 1] != L'/') { // 仅对开始标签（非 </x>）生效
                    bool isScript = (tagName.size() == 6);
                    bool isStyle = (tagName.size() == 5);
                    const wchar_t* s6 = L"script", * s5 = L"style";
                    if (isScript) {
                        for (int q = 0; q < 6; q++)
                            if (towlower(tagName[q]) != s6[q]) {
                                isScript = false;
                                break;
                            }
                    }
                    if (isStyle) {
                        for (int q = 0; q < 5; q++)
                            if (towlower(tagName[q]) != s5[q]) {
                                isStyle = false;
                                break;
                            }
                    }
                    if (isScript)
                        inBlock = 1;
                    else if (isStyle)
                        inBlock = 2;
                }
                i = j;
                continue;
            }
            // HTML 中的括号也做彩虹着色
            if (c == L'(' || c == L'[' || c == L'{') {
                int col = (unsigned char)(depth % 6);
                add(i, 1, T_BRACKET, col);
                depth++;
                i++;
                continue;
            }
            if (c == L')' || c == L']' || c == L'}') {
                int d = depth > 0 ? depth - 1 : 0;
                int col = (unsigned char)(d % 6);
                add(i, 1, T_BRACKET, col);
                if (depth > 0) depth--;
                i++;
                continue;
            }
            add(i, 1, T_TEXT, 0);
            i++;
            continue;
        }
        // ---- 行注释（// 或 -- 或 #）直到行尾 ----
        if (lineComment && lcChar == L'/' && i + 1 < n && s[i] == L'/' && s[i + 1] == L'/') {
            add(i, n - i, T_COMMENT, 0);
            i = n;
            continue;
        }
        if (lineComment && lcChar == L'-' && i + 1 < n && s[i] == L'-' && s[i + 1] == L'-') {
            add(i, n - i, T_COMMENT, 0);
            i = n;
            continue;
        }
        if (lineComment && lcChar == L'#' && c == L'#') {
            add(i, n - i, T_COMMENT, 0);
            i = n;
            continue;
        }
        // ---- 块注释 /* */ ----
        if (blockCmt && i + 1 < n && s[i] == L'/' && s[i + 1] == L'*') {
            int j = i + 2;
            while (j + 1 < n && !(s[j] == L'*' && s[j + 1] == L'/')) j++;
            int end = (j + 1 < n) ? j + 2 : n;
            add(i, end - i, T_COMMENT, 0);
            if (j + 1 < n)
                inBC = false;
            else
                inBC = true; // 本行内闭合则离开；否则标记跨行
            i = end;
            continue;
        }
        // ---- Python 三引号块字符串 """ / ''' ----
        if (lang == L_PY && i + 2 < n && s[i] == L'"' && s[i + 1] == L'"' && s[i + 2] == L'"') {
            int j = i + 3;
            while (j + 2 < n && !(s[j] == L'"' && s[j + 1] == L'"' && s[j + 2] == L'"')) j++;
            int end = (j + 2 < n) ? j + 3 : n;
            add(i, end - i, T_STRING, 0);
            i = end;
            continue;
        }
        // ---- 普通字符串字面量（按语言区分定界符） ----
        bool isStr = false;
        wchar_t q = 0;
        if (lang == L_CS || lang == L_JS || lang == L_C || lang == L_CPP || lang == L_JAVA) {
            if (c == L'"' || c == L'\'' || (lang == L_JS && c == L'`')) {
                q = c;
                isStr = true;
            } // JS 额外支持模板字符串 `
        }
        else if (lang == L_SQL) {
            if (c == L'\'') {
                q = c;
                isStr = true;
            } // SQL 仅单引号
        }
        else if (lang == L_PY) {
            if (c == L'"' || c == L'\'') {
                q = c;
                isStr = true;
            }
        }
        else if (lang == L_JSON) {
            if (c == L'"') {
                q = c;
                isStr = true;
            } // JSON 仅双引号
        }
        else if (lang == L_CSS) {
            if (c == L'"' || c == L'\'') {
                q = c;
                isStr = true;
            }
        }
        if (isStr) {
            int j = i + 1;
            if (lang == L_SQL) {
                // SQL 中两个单引号 '' 表示转义的单引号，需跳过
                while (j < n) {
                    if (s[j] == L'\'') {
                        if (j + 1 < n && s[j + 1] == L'\'') {
                            j += 2;
                            continue;
                        }
                        else {
                            j++;
                            break;
                        }
                    }
                    j++;
                }
            }
            else {
                // 其它语言：处理转义符 \x，遇引号结束，遇换行（非 JS 模板串）停止
                while (j < n) {
                    if (s[j] == L'\\' && j + 1 < n) {
                        j += 2;
                        continue;
                    }
                    if (s[j] == q) {
                        j++;
                        break;
                    }
                    if (s[j] == L'\n') break;
                    j++;
                }
            }
            add(i, j - i, T_STRING, 0);
            i = j;
            continue;
        }
        // ---- 数字字面量（含十六进制、科学计数、类型后缀） ----
        if (isClike || lang == L_PY) {
            if (iswdigit(c) || (c == L'.' && i + 1 < n && iswdigit(s[i + 1]))) {
                int j = i;
                while (j < n) {
                    wchar_t ch = s[j];
                    if (iswdigit(ch) || ch == L'.' || ch == L'_' || ch == L'\'' || ch == L'x' || ch == L'X') {
                        j++;
                        continue;
                    }
                    if ((ch >= L'a' && ch <= L'f') || (ch >= L'A' && ch <= L'F') || ch == L'u' ||
                        ch == L'U' || ch == L'l' || ch == L'L' || ch == L'p' || ch == L'P') {
                        j++;
                        continue;
                    }
                    break;
                }
                add(i, j - i, T_NUMBER, 0);
                i = j;
                continue;
            }
        }
        // ---- 标识符（含关键字/类型/函数名判定） ----
        if (iswalpha(c) || c == L'_' || c == L'$' || (lang == L_PY && c == L'@')) {
            int j = i;
            while (j < n &&
                (iswalnum(s[j]) || s[j] == L'_' || s[j] == L'$' || (lang == L_PY && s[j] == L'@')))
                j++;
            std::wstring w(s + i, j - i);
            // SQL：BEGIN / CASE 当作块左括号，END 当作块右括号，按 depth 做彩虹着色并维护深度，
            // 这样它们能与 ()[]{} 一起形成正确嵌套的配对高亮（如
            // CASE...END、BEGIN...END），而非普通关键字。
            if (lang == L_SQL && (w == L"BEGIN" || w == L"END" || w == L"CASE")) {
                if (w == L"END") {
                    int d = depth > 0 ? depth - 1 : 0;
                    int col = (unsigned char)(d % 6);
                    add(i, j - i, T_BRACKET, col);
                    if (depth > 0) depth--;
                } // 右括号（END 关闭 BEGIN 或 CASE）
                else {
                    int col = (unsigned char)(depth % 6);
                    add(i, j - i, T_BRACKET, col);
                    depth++;
                } // 左括号 BEGIN / CASE
                i = j;
                continue;
            }
            TokType t = T_IDENT;
            // 依据语言查关键字表/类型表，并把 PascalCase 词判定为类型
            if (lang == L_CS) {
                if (KW_CS.count(w))
                    t = T_KEYWORD;
                else if (TY_CS.count(w))
                    t = T_TYPE;
                else if (isUpperWord(w))
                    t = T_TYPE; // PascalCase => 类型/类名/变量类型
            }
            else if (lang == L_SQL) {
                if (KW_SQL.count(w)) t = T_KEYWORD;
            }
            else if (lang == L_JS) {
                if (KW_JS.count(w))
                    t = T_KEYWORD;
                else if (isUpperWord(w))
                    t = T_TYPE; // 构造函数/类名
            }
            else if (lang == L_PY) {
                if (KW_PY.count(w)) t = T_KEYWORD;
            }
            else if (lang == L_CSS) {
                if (KW_CSS.count(w)) t = T_KEYWORD;
            }
            else if (lang == L_JSON) {
                if (w == L"true" || w == L"false" || w == L"null") t = T_KEYWORD;
            }
            else if (lang == L_C) {
                if (KW_C.count(w))
                    t = T_KEYWORD;
                else if (TY_C.count(w))
                    t = T_TYPE;
                else if (isUpperWord(w))
                    t = T_TYPE; // PascalCase => 类型/类名
            }
            else if (lang == L_CPP) {
                if (KW_CPP.count(w))
                    t = T_KEYWORD;
                else if (TY_CPP.count(w))
                    t = T_TYPE;
                else if (isUpperWord(w))
                    t = T_TYPE; // PascalCase => 类型/类名
            }
            else if (lang == L_JAVA) {
                if (KW_JAVA.count(w))
                    t = T_KEYWORD;
                else if (TY_JAVA.count(w))
                    t = T_TYPE;
                else if (isUpperWord(w))
                    t = T_TYPE; // PascalCase => 类型/类名
            }
            // EXEC / PROCEDURE 后的标识符 => 存储过程名（支持 dbo.procName 形式）
            int d2 = j;
            while (d2 < n && (s[d2] == L' ' || s[d2] == L'\t')) d2++;
            bool followedByDot = (d2 < n && s[d2] == L'.');
            if (lang == L_SQL && afterProcKw && t == T_IDENT) {
                if (followedByDot) {
                    afterProcKw = false;
                    afterProcDot = true;
                } // schema 限定名，跳过 schema 部分
                else {
                    t = T_PROC;
                    afterProcKw = false;
                }
            }
            // 函数/存储过程调用：标识符后紧跟 '('（忽略空白）
            int k = j;
            while (k < n && (s[k] == L' ' || s[k] == L'\t')) k++;
            if (k < n && s[k] == L'(') {
                if (lang == L_SQL)
                    t = T_PROC;              // SQL 中任何后接 '(' 的标识符视为函数/过程
                else if (t == T_KEYWORD) { /* C#/JS 控制流关键字(if/for/while...)保持关键字色 */
                }
                else if (t == T_TYPE) {  /* 构造函数(new List())保持类型色 */
                }
                else
                    t = T_FUNC;
            }
            add(i, j - i, t, 0);
            if (lang == L_SQL && (w == L"EXEC" || w == L"EXECUTE" || w == L"PROCEDURE"))
                afterProcKw = true;
            i = j;
            continue;
        }
        // ---- 括号（彩虹着色）与标点 ----
        if (c == L'(' || c == L'[' || c == L'{') {
            int col = (unsigned char)(depth % 6);
            add(i, 1, T_BRACKET, col);
            depth++;
            i++;
            continue;
        }
        if (c == L')' || c == L']' || c == L'}') {
            int d = depth > 0 ? depth - 1 : 0;
            int col = (unsigned char)(d % 6);
            add(i, 1, T_BRACKET, col);
            if (depth > 0) depth--;
            i++;
            continue;
        }
        if (c == L'{' || c == L'}') {
            add(i, 1, T_PUNCT, 0);
            i++;
            continue;
        }
        if (c == L'.') {
            if (afterProcDot) {
                afterProcDot = false;
                afterProcKw = true;
            } // schema 之后真正的 proc 名
            add(i, 1, T_PUNCT, 0);
            i++;
            continue;
        }
        add(i, 1, T_TEXT, 0);
        i++; // 其它字符按普通文本处理
    }
}

// ----------------------------------------------------------------------------
// 加载文本
// ----------------------------------------------------------------------------
// 根据当前字号与屏幕 DPI 创建等宽字体，并测量出真实字符宽/行高。
// 用整串文本宽度求平均字符宽，比系统 tmAveCharWidth 更准确，避免长行偏移累积。
void ensureSideFont(); // 前向声明（定义在 ensureFont 之后）
void ensureFont() {
    invalidateLineCache(); // 字体/字号即将变化，先释放所有行缓存，避免旧字体句柄悬空
    if (g_hFont) DeleteObject(g_hFont);
    HDC hdc = GetDC(g_hwnd);
    int h = -MulDiv(g_fontSize, GetDeviceCaps(hdc, LOGPIXELSY),
        72); // 字号(pt)转设备像素高度（取负表示字符高度）
    LOGFONT lf = { 0 };
    lf.lfHeight = h;
    lf.lfWeight = FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    wcscpy_s(lf.lfFaceName, L"Consolas"); // 首选 Consolas
    g_hFont = CreateFontIndirect(&lf);
    if (!g_hFont) {
        wcscpy_s(lf.lfFaceName, L"Lucida Console");
        g_hFont = CreateFontIndirect(&lf);
    } // 回退1
    if (!g_hFont) {
        wcscpy_s(lf.lfFaceName, L"Courier New");
        g_hFont = CreateFontIndirect(&lf);
    } // 回退2
    SelectObject(hdc, g_hFont);
    TEXTMETRIC tm;
    GetTextMetrics(hdc, &tm);
    // 用整串宽度求平均字宽，比 tmAveCharWidth 更准，避免整行偏移累积
    const wchar_t* probe = L"the quick brown fox jumps over 0123456789";
    SIZE sz;
    GetTextExtentPoint32(hdc, probe, (int)wcslen(probe), &sz);
    g_charW = (int)((double)sz.cx / (double)wcslen(probe) + 0.5);
    if (g_charW < 1) g_charW = tm.tmAveCharWidth;
    g_lineH = tm.tmHeight + tm.tmExternalLeading + 2;
    ReleaseDC(g_hwnd, hdc);
    g_lineW.assign(g_lineCount, -1);
    g_maxLineW = 0;
    seedMaxLineW();   // 字号/字体变化后像素宽随之变化：行宽缓存失效，重新采样估算
    ensureSideFont(); // 侧栏字体固定，但需确保已创建（首次）
}

// 侧栏（文件夹列表）字体：固定字号，不随主编辑框的 Ctrl+滚轮 缩放而变化。
void ensureSideFont() {
    if (g_sideFont) return; // 只创建一次
    HDC hdc = GetDC(g_hwnd);
    int h = -MulDiv(12, GetDeviceCaps(hdc, LOGPIXELSY), 72); // 固定 12pt
    LOGFONT lf = { 0 };
    lf.lfHeight = h;
    lf.lfWeight = FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    wcscpy_s(lf.lfFaceName, L"Segoe UI");
    g_sideFont = CreateFontIndirect(&lf);
    if (!g_sideFont) {
        wcscpy_s(lf.lfFaceName, L"Microsoft YaHei");
        g_sideFont = CreateFontIndirect(&lf);
    }
    if (!g_sideFont) {
        wcscpy_s(lf.lfFaceName, L"Tahoma");
        g_sideFont = CreateFontIndirect(&lf);
    }
    ReleaseDC(g_hwnd, hdc);
}

// 重新切分行为“逻辑行”数组。跨行着色状态（块注释/括号深度/ASPX 服务端块/块字符串引号）
// 不再在此全量扫描，而是改为按需计算（见 ensureLineState），载入大文件不再卡死。
void rebuildLines() {
    invalidateLineCache(); // 文本内容即将改变，行缓存全部失效
    g_lineStart.clear();
    g_lineLen.clear();
    g_lineDepth.clear();
    g_lineInBC.clear();
    g_lineInSrv.clear();
    g_lineInBlock.clear();
    g_lineBsQ.clear();
    g_tokens.clear();
    int n = (int)g_text.size();
    int i = 0;
    int line = 0;
    g_lineStart.push_back(0); // 第 0 行从偏移 0 开始
    for (i = 0; i < n; i++) {
        if (g_text[i] == L'\n') {
            g_lineLen.push_back(i - g_lineStart[line]); // 记录当前行长度（不含 \n）
            line++;
            g_lineStart.push_back(i + 1); // 下一行从 \n 之后开始
        }
    }
    g_lineLen.push_back(n - g_lineStart.back()); // 最后一行（可能没有末尾换行）
    g_lineCount = (int)g_lineStart.size();
    g_lineDepth.assign(g_lineCount, 0);
    g_lineInBC.assign(g_lineCount, false);
    g_lineInSrv.assign(g_lineCount, false);
    g_lineBsQ.assign(g_lineCount, 0);
    g_lineInBlock.assign(g_lineCount, 0);
    g_stateDone.assign(g_lineCount, 0); // 跨行状态改为按需计算，载入时不扫描
    g_lineW.assign(g_lineCount, -1);    // 行宽懒缓存：-1 表示尚未计算
    g_maxLineW = 0;
    g_stateDone[0] = 1; // 第 0 行起始状态即全局初始状态（全 0/false），标记已就绪
    seedMaxLineW();     // 采样估算最大行宽，给出初始水平滚动范围（不再全文件扫描）
}

// 按需计算第 l 行起始的跨行着色状态：从其前最近一个“状态已就绪”的行 k 起，
// 用 scanLine（out=nullptr）向前推进状态直到 l，逐行写入起始状态并标记已就绪。
// 载入时不再全文件扫描；首次滚动/着色到某区域才按需补齐，避免大文件卡死。
void ensureLineState(int l) {
    if (l < 0) l = 0;
    if (l >= g_lineCount) l = g_lineCount - 1;
    if (g_stateDone[l]) return; // 已就绪，直接返回
    int k = l - 1;
    while (k >= 0 && !g_stateDone[k]) k--; // 找最近的已就绪行（状态总是顺序补齐，k 之前必全就绪）
    if (k < 0) k = 0;
    bool inBC = false, inSrv = false;
    int inBlock = 0;
    wchar_t bsQ = 0;
    int depth = 0; // 第 0 行起始状态恒为全局初始
    if (k > 0) {
        inBC = g_lineInBC[k];
        inBlock = g_lineInBlock[k];
        inSrv = g_lineInSrv[k];
        bsQ = g_lineBsQ[k];
        depth = g_lineDepth[k];
    }
    for (int m = k; m < l; m++) { // 顺序向前推进，逐行写回起始状态
        int st = g_lineStart[m], en = st + g_lineLen[m];
        scanLine(g_text.c_str() + st, en - st, g_langId, inBC, inBlock, inSrv, bsQ, depth, nullptr);
        g_lineDepth[m + 1] = depth;
        g_lineInBC[m + 1] = inBC;
        g_lineInSrv[m + 1] = inSrv;
        g_lineInBlock[m + 1] = inBlock;
        g_lineBsQ[m + 1] = bsQ;
        g_stateDone[m + 1] = 1;
    }
}
// 获取第 l 行的 token 列表；若尚未解析则先确保起始状态就绪，再懒解析（续行着色）。
const std::vector<Token>& lineTokens(int l) {
    auto it = g_tokens.find(l);
    if (it != g_tokens.end()) return it->second;
    ensureLineState(l); // 确保该行起始状态已就绪（按需向前扫描）
    bool inBC = g_lineInBC[l];
    int inBlock = g_lineInBlock[l];
    bool inSrv = g_lineInSrv[l];
    wchar_t bsQ = g_lineBsQ[l];
    int depth = g_lineDepth[l];
    std::vector<Token>& toks = g_tokens[l];
    toks.clear();
    int st = g_lineStart[l], en = st + g_lineLen[l];
    scanLine(g_text.c_str() + st, en - st, g_langId, inBC, inBlock, inSrv, bsQ, depth, &toks);
    return toks;
}

// ----------------------------------------------------------------------------
// 视觉行（自动换行）
// 把一个“逻辑行”按可用宽度拆成多个“视觉行”，写入 g_visual。
// 不换行时一行对应一个视觉行；换行时按字符累计宽度切分。
// ----------------------------------------------------------------------------
void buildVisual() {
    g_visual.clear();
    int clientW = 0;
    if (g_hwnd) {
        RECT r;
        GetClientRect(g_hwnd, &r);
        clientW = r.right - r.left;
    }
    int avail = g_wrap ? (clientW - leftBar() - g_gutterW) : 0; // 换行模式下的可用文本宽度（像素）
    if (avail < g_charW) avail = g_charW;
    for (int l = 0; l < g_lineCount; l++) {
        int len = g_lineLen[l];
        if (!g_wrap) { // 非换行：视觉行与窗口宽度无关，整行直接作为一个视觉行
            g_visual.push_back({ l, 0, len });
        }
        else {
            int pw = linePx(l); // 换行：用（懒缓存）行宽判断是否需切分
            if (pw <= avail) {
                g_visual.push_back({ l, 0, len }); // 整行作为一个视觉行
            }
            else {
                int start = 0;
                while (start < len) {
                    int col = start, ww = 0;
                    while (col < len) {
                        int cw = charW(g_text[g_lineStart[l] + col]);
                        if (ww + cw > avail && col > start) break; // 超过可用宽度则在此处断行（至少放一个字符）
                        ww += cw;
                        col++;
                    }
                    g_visual.push_back({ l, start, col - start }); // 视觉行：所属逻辑行、起始列、长度
                    start = col;
                }
            }
        }
    }
    g_visualCount = (int)g_visual.size();
}

// ----------------------------------------------------------------------------
// 括号配对
// 在光标处若处于一个括号字符上，则向同方向（前/后）寻找匹配的括号（用栈深度匹配）。
// 结果写入 g_matchA / g_matchB（分别为左右括号的字符偏移），供绘制高亮。
// ----------------------------------------------------------------------------
void findMatch() {
    g_matchA = -1;
    g_matchB = -1;
    if (g_caretOff < 0 || g_caretOff >= (int)g_text.size()) return;
    wchar_t c = g_text[g_caretOff];
    // 字符括号：() [] {} 必须“同类型”配对（修正大中小括号混合嵌套时把 [ 与 ) 配错的问题）
    static const wchar_t pairs[3][2] = { {L'(', L')'}, {L'[', L']'}, {L'{', L'}'} };
    for (int p = 0; p < 3; p++) {
        if (c == pairs[p][0]) { // 光标在左括号：向右找同类型右括号
            int stk = 1;
            for (int i = g_caretOff + 1; i < (int)g_text.size(); i++) {
                if (g_text[i] == pairs[p][0])
                    stk++;
                else if (g_text[i] == pairs[p][1]) {
                    stk--;
                    if (stk == 0) {
                        g_matchA = g_caretOff;
                        g_matchAw = 1;
                        g_matchB = i;
                        g_matchBw = 1;
                        return;
                    }
                }
            }
            return;
        }
        if (c == pairs[p][1]) { // 光标在右括号：向左找同类型左括号
            int stk = 1;
            for (int i = g_caretOff - 1; i >= 0; i--) {
                if (g_text[i] == pairs[p][1])
                    stk++;
                else if (g_text[i] == pairs[p][0]) {
                    stk--;
                    if (stk == 0) {
                        g_matchA = i;
                        g_matchAw = 1;
                        g_matchB = g_caretOff;
                        g_matchBw = 1;
                        return;
                    }
                }
            }
            return;
        }
    }
    // 词括号：SQL 的 BEGIN / END —— 整词匹配，按栈配对，像 () 一样高亮对应块的起止
    if (g_langId == L_SQL) {
        int ws, we;
        wordAtOffset(g_caretOff, ws, we);
        if (we > ws) {
            std::wstring w = g_text.substr(ws, we - ws);
            if (w == L"BEGIN" || w == L"END" || w == L"CASE") {
                // 判断某偏移所在整词：BEGIN 返回 1，END 返回 -1，其它返回 0
                auto wordKind = [&](int pos) -> int {
                    if (pos < 0 || pos >= (int)g_text.size()) return 0;
                    int a = pos;
                    while (a > 0 && isWordChar(g_text[a - 1])) a--;
                    int b = pos;
                    while (b < (int)g_text.size() && isWordChar(g_text[b])) b++;
                    std::wstring ww = g_text.substr(a, b - a);
                    if (ww == L"BEGIN" || ww == L"CASE") return 1;
                    if (ww == L"END") return -1;
                    return 0;
                    };
                if (w == L"BEGIN") { // 向右找匹配的 END（自身已占一个 open，栈初值 1）
                    int stk = 1;
                    for (int i = we; i < (int)g_text.size();) {
                        while (i < (int)g_text.size() && !isWordChar(g_text[i])) i++;
                        if (i >= (int)g_text.size()) break;
                        int a = i;
                        while (i < (int)g_text.size() && isWordChar(g_text[i])) i++;
                        int k = wordKind(a);
                        if (k == 1)
                            stk++;
                        else if (k == -1) {
                            stk--;
                            if (stk == 0) {
                                g_matchA = ws;
                                g_matchAw = we - ws;
                                int be = a;
                                while (be < (int)g_text.size() && isWordChar(g_text[be])) be++;
                                g_matchB = a;
                                g_matchBw = be - a;
                                return;
                            }
                        }
                    }
                }
                else { // END 向左找匹配的 BEGIN（自身已占一个 close，栈初值 1）
                    int stk = 1;
                    for (int i = ws - 1; i >= 0;) {
                        while (i >= 0 && !isWordChar(g_text[i])) i--;
                        if (i < 0) break;
                        int b = i;
                        while (b >= 0 && isWordChar(g_text[b])) b--;
                        b++;
                        int k = wordKind(b);
                        if (k == -1)
                            stk++;
                        else if (k == 1) {
                            stk--;
                            if (stk == 0) {
                                int be = b;
                                while (be < (int)g_text.size() && isWordChar(g_text[be])) be++;
                                g_matchA = b;
                                g_matchAw = be - b;
                                g_matchB = ws;
                                g_matchBw = we - ws;
                                return;
                            }
                        }
                        i = b - 1;
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
void setCaret(int off) {
    g_caretOff = off;
    if (g_caretOff < 0) g_caretOff = 0;
    if (g_caretOff > (int)g_text.size()) g_caretOff = (int)g_text.size();
    findMatch();
    int line = lineOfOffset(g_caretOff);
    int vline = -1;
    // 找到光标所在（按精确字符范围匹配）的视觉行
    for (int v = 0; v < g_visualCount; v++) {
        if (g_visual[v].line == line && g_caretOff >= g_lineStart[line] &&
            g_caretOff <= g_lineStart[line] + g_lineLen[line]) {
            vline = v;
            break;
        }
    }
    if (vline < 0) {
        for (int v = 0; v < g_visualCount; v++)
            if (g_visual[v].line == line) {
                vline = v;
                break;
            }
    } // 退而求其次按行找
    RECT r;
    GetClientRect(g_hwnd, &r);
    int edH = r.bottom - editorTop();
    int visH = edH / g_lineH; // 可见视觉行数
    if (vline >= 0) {
        // 垂直滚动：若光标在可见区上方则顶到它，在下方则翻页使其可见
        if (vline < g_topLine)
            g_topLine = vline;
        else if (vline >= g_topLine + visH)
            g_topLine = vline - visH + 1;
    }
    int col = g_caretOff - g_lineStart[line];
    int px = linePrefixPx(line, col);                    // 光标在行内的像素 x
    int caretX = leftBar() + g_gutterW + px - g_scrollX; // 屏幕坐标 x
    if (caretX < leftBar() + g_gutterW)
        g_scrollX = px; // 光标跑到左侧行号区外 => 左移
    else if (caretX > r.right - 20)
        g_scrollX = leftBar() + g_gutterW + px - (r.right - 20); // 右侧溢出 => 右移
    if (g_scrollX < 0) g_scrollX = 0;
    updateScroll();
    InvalidateRect(g_hwnd, NULL, TRUE); // 触发重绘
    updateCaretPos();                   // 移动系统光标
}
// 根据当前 g_caretOff 计算光标在屏幕上的 (x,y)，并创建/移动系统插入符（caret）。
void updateCaretPos() {
    if (!g_hwnd) return;
    int line = lineOfOffset(g_caretOff);
    int col = g_caretOff - g_lineStart[line];
    int vline = -1;
    for (int v = 0; v < g_visualCount; v++) {
        if (g_visual[v].line == line && g_caretOff >= g_lineStart[line] &&
            g_caretOff <= g_lineStart[line] + g_lineLen[line]) {
            vline = v;
            break;
        }
    }
    if (vline < 0) {
        for (int v = 0; v < g_visualCount; v++)
            if (g_visual[v].line == line) {
                vline = v;
                break;
            }
    }
    if (vline < 0) return;
    int y = editorTop() + (vline - g_topLine) * g_lineH; // 屏幕 y（含标签栏/查找条偏移）
    int x;
    if (g_wrap) {
        x = leftBar() + g_gutterW + linePrefixPx(line, g_caretOff - g_lineStart[line]) -
            linePrefixPx(line, g_visual[vline].col);
    }
    else {
        x = leftBar() + g_gutterW + linePrefixPx(line, g_caretOff - g_lineStart[line]) - g_scrollX;
    } // 非换行按水平滚动偏移
    DestroyCaret();
    if (!g_caretBmp) { // 惰性创建与主题色一致的纯色光标位图
        int cw = 2, ch = g_lineH - 2;
        if (ch < 1) ch = 1;
        HDC hdc = GetDC(g_hwnd), mdc = CreateCompatibleDC(hdc);
        ReleaseDC(g_hwnd, hdc);
        g_caretBmp = CreateCompatibleBitmap(mdc, cw, ch);
        HBITMAP ob = (HBITMAP)SelectObject(mdc, g_caretBmp);
        HBRUSH br = CreateSolidBrush(TH.caret);
        HBRUSH obr = (HBRUSH)SelectObject(mdc, br);
        RECT rc = { 0, 0, cw, ch };
        FillRect(mdc, &rc, br);
        SelectObject(mdc, ob);
        SelectObject(mdc, obr);
        DeleteObject(br);
        DeleteDC(mdc);
    }
    if (CreateCaret(g_hwnd, g_caretBmp, 0, 0)) { // 传入位图后宽高参数被忽略
        SetCaretPos(x, y);
        ShowCaret(g_hwnd);
    }
}
// 根据当前文本行数与可见高度，重设垂直/水平滚动条的范围与位置。
void updateScroll() {
    // 安全网：确保滚动位置始终合法（防止其它路径误设导致负数下标访问 g_visual）
    if (g_topLine < 0) g_topLine = 0;
    if (g_visualCount > 0 && g_topLine > g_visualCount - 1) g_topLine = g_visualCount - 1;
    RECT r;
    GetClientRect(g_hwnd, &r);
    int edH = r.bottom - editorTop();
    if (edH < 1) edH = 1;
    int visH = edH / g_lineH;
    SCROLLINFO si = { sizeof(si) };
    si.fMask = SIF_ALL;
    si.nMin = 0;
    si.nMax = g_visualCount - 1;
    si.nPage = visH;
    si.nPos = g_topLine;
    SetScrollInfo(g_hwnd, SB_VERT, &si, TRUE);
    int maxX = g_maxLineW; // 最长行的像素宽（缓存，避免每次滚动全量扫描）
    int clientW = r.right - r.left;
    int hmax = maxX - (clientW - leftBar() - g_gutterW); // 最大水平滚动量 = 最长行宽 - 可视文本宽
    if (hmax < 0) hmax = 0;
    SCROLLINFO hi = { sizeof(hi) };
    hi.fMask = SIF_ALL;
    hi.nMin = 0;
    hi.nMax = hmax;
    hi.nPage = clientW - g_gutterW;
    hi.nPos = g_scrollX;
    SetScrollInfo(g_hwnd, SB_HORZ, &hi, TRUE);
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
    std::wstring name;     // 显示名
    std::wstring fullPath; // 完整路径
    bool isDir = false;    // 是否为文件夹
    bool expanded = false; // 文件夹是否展开
    bool loaded = false;   // 子项是否已枚举
    int depth = 0;         // 缩进层级（根的直接子项=0）
    std::vector<TreeNode> children;
};
TreeNode g_treeRoot;                // 根文件夹节点
bool g_folderOpen = false;          // 是否已打开文件夹（决定侧栏是否显示）
std::wstring g_folderPath;          // 根文件夹路径
std::vector<TreeNode*> g_treeRows; // 当前可见行（已展开层级扁平化后的指针列表）
int g_treeScroll = 0;               // 树垂直滚动像素
int g_sidebarHover = -1;            // 侧栏悬停行索引
int g_sideHoverBtn = 0;             // 侧栏头部按钮悬停（1=关闭×）
bool g_treeDrag = false;            // 是否正在拖动树滚动条滑块
int g_treeDragGrab = 0;             // 拖动时鼠标与滑块顶部的偏移

// 侧栏拖拽调宽状态
int g_sidebarW = SIDEBAR_W;     // 侧栏当前宽度（可被鼠标拖动调整）
bool g_sidebarResizing = false; // 是否正在拖动侧栏右缘调宽
int g_sidebarResizeStartX = 0;  // 拖拽起始鼠标 x
int g_sidebarResizeStartW = 0;  // 拖拽起始侧栏宽度

// 标签栏水平滚动偏移（标签总宽超过可视区时，让当前标签可见）
int g_tabScroll = 0;
int g_ctxTab = -1; // 右键标签菜单所针对的标签索引（在弹出菜单前写入，WM_COMMAND 时读取）

// 左侧栏占用的宽度：未打开文件夹时为 0，编辑器与行号区占满整个客户区
int leftBar() {
    return g_folderOpen ? g_sidebarW : 0;
}

// 枚举目录子项：先文件夹后文件，各自按名称排序，结果写入 node.children
void loadDir(TreeNode& node) {
    node.children.clear();
    WIN32_FIND_DATA fd;
    HANDLE h = FindFirstFile((node.fullPath + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        node.loaded = true;
        return;
    }
    std::vector<TreeNode> dirs, files;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        TreeNode c;
        c.name = fd.cFileName;
        c.fullPath = node.fullPath + L"\\" + fd.cFileName;
        c.isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        c.depth = node.depth + 1;
        if (c.isDir)
            dirs.push_back(c);
        else
            files.push_back(c);
    } while (FindNextFile(h, &fd));
    FindClose(h);
    auto cmp = [](const TreeNode& a, const TreeNode& b) { return a.name < b.name; };
    std::sort(dirs.begin(), dirs.end(), cmp);
    std::sort(files.begin(), files.end(), cmp);
    node.children.insert(node.children.end(), dirs.begin(), dirs.end());
    node.children.insert(node.children.end(), files.begin(), files.end());
    node.loaded = true;
}
// 递归把已展开目录的子项压入可见行列表
void pushTreeRows(TreeNode& node) {
    for (auto& c : node.children) {
        g_treeRows.push_back(&c);
        if (c.isDir && c.expanded) pushTreeRows(c);
    }
}
// 重建可见行：根的直接子项 + 所有已展开目录的子项
void rebuildTreeRows() {
    g_treeRows.clear();
    for (auto& c : g_treeRoot.children) {
        g_treeRows.push_back(&c);
        if (c.isDir && c.expanded) pushTreeRows(c);
    }
}
// 限制树滚动范围（不越界）
void clampTreeScroll() {
    RECT r;
    GetClientRect(g_hwnd, &r);
    int sbTop = editorTop() + SIDEBAR_HEAD_H;
    int viewH = r.bottom - sbTop;
    if (viewH < 0) viewH = 0;
    int contentH = (int)g_treeRows.size() * SIDEBAR_ROW_H;
    int maxS = contentH > viewH ? contentH - viewH : 0;
    if (g_treeScroll < 0) g_treeScroll = 0;
    if (g_treeScroll > maxS) g_treeScroll = maxS;
}
// 打开文件夹：载入根目录并展开，激活侧栏，编辑器整体右移让位
void openFolder(const std::wstring& path) {
    if (path.empty()) return;
    g_folderPath = path;
    g_treeRoot = TreeNode();
    g_treeRoot.name = path.substr(path.find_last_of(L'\\') + 1);
    if (g_treeRoot.name.empty()) g_treeRoot.name = path; // 驱动器根目录（如 C:\）时兜底显示完整路径
    g_treeRoot.fullPath = path;
    g_treeRoot.isDir = true;
    g_treeRoot.depth = -1;
    g_treeRoot.expanded = true;
    loadDir(g_treeRoot);
    g_folderOpen = true;
    rebuildTreeRows();
    g_treeScroll = 0;
    clampTreeScroll();
    buildVisual();
    updateScroll();
    InvalidateRect(g_hwnd, NULL, TRUE);
}
// 关闭文件夹：隐藏侧栏，编辑器占满
void closeFolder() {
    g_folderOpen = false;
    g_treeRows.clear();
    g_treeScroll = 0;
    g_sidebarHover = -1;
    g_sideHoverBtn = 0;
    buildVisual();
    updateScroll();
    InvalidateRect(g_hwnd, NULL, TRUE);
}
// 文件夹选择对话框（SHBrowseForFolder）
std::wstring openFolderDialog() {
    BROWSEINFO bi = { 0 };
    bi.hwndOwner = g_hwnd;
    bi.lpszTitle = L"选择要打开的文件夹";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolder(&bi);
    if (!pidl) return L"";
    wchar_t buf[MAX_PATH] = { 0 };
    if (!SHGetPathFromIDList(pidl, buf)) {
        CoTaskMemFree(pidl);
        return L"";
    }
    CoTaskMemFree(pidl);
    return std::wstring(buf);
}
// 小图标：文件夹
void drawFolderIcon(HDC hdc, int x, int y, COLORREF col) {
    HPEN p = CreatePen(PS_SOLID, 1, col);
    HPEN op = (HPEN)SelectObject(hdc, p);
    HBRUSH b = CreateSolidBrush(col);
    HBRUSH ob = (HBRUSH)SelectObject(hdc, b);
    POINT pts[6];
    pts[0].x = x;
    pts[0].y = y + 3;
    pts[1].x = x + 4;
    pts[1].y = y + 3;
    pts[2].x = x + 6;
    pts[2].y = y + 5;
    pts[3].x = x + 13;
    pts[3].y = y + 5;
    pts[4].x = x + 13;
    pts[4].y = y + 13;
    pts[5].x = x;
    pts[5].y = y + 13;
    Polygon(hdc, pts, 6);
    SelectObject(hdc, op);
    DeleteObject(p);
    SelectObject(hdc, ob);
    DeleteObject(b);
}
// 小图标：文件（带折角）
void drawFileIcon(HDC hdc, int x, int y, COLORREF col) {
    HPEN p = CreatePen(PS_SOLID, 1, col);
    HPEN op = (HPEN)SelectObject(hdc, p);
    HBRUSH b = CreateSolidBrush(col);
    HBRUSH ob = (HBRUSH)SelectObject(hdc, b);
    Rectangle(hdc, x, y, x + 11, y + 13);
    POINT pts[3];
    pts[0].x = x + 6;
    pts[0].y = y;
    pts[1].x = x + 11;
    pts[1].y = y;
    pts[2].x = x + 11;
    pts[2].y = y + 5;
    Polygon(hdc, pts, 3);
    SelectObject(hdc, op);
    DeleteObject(p);
    SelectObject(hdc, ob);
    DeleteObject(b);
}
// 小图标：展开/折叠三角
void drawTreeTri(HDC hdc, int x, int y, bool down, COLORREF col) {
    HPEN p = CreatePen(PS_SOLID, 1, col);
    HPEN op = (HPEN)SelectObject(hdc, p);
    HBRUSH b = CreateSolidBrush(col);
    HBRUSH ob = (HBRUSH)SelectObject(hdc, b);
    POINT pts[3];
    if (down) {
        pts[0].x = x;
        pts[0].y = y - 3;
        pts[1].x = x + 7;
        pts[1].y = y - 3;
        pts[2].x = x + 3;
        pts[2].y = y + 3;
    }
    else {
        pts[0].x = x;
        pts[0].y = y - 3;
        pts[1].x = x + 6;
        pts[1].y = y;
        pts[2].x = x;
        pts[2].y = y + 3;
    }
    Polygon(hdc, pts, 3);
    SelectObject(hdc, op);
    DeleteObject(p);
    SelectObject(hdc, ob);
    DeleteObject(b);
}
// 侧栏鼠标按下：关闭按钮 / 滚动条滑块与轨道 / 行点击（目录展开折叠、文件打开）
void sidebarDown(int x, int y) {
    RECT r;
    GetClientRect(g_hwnd, &r);
    int lb = leftBar();
    // 头部关闭按钮 ×
    RECT cr = { lb - 22, editorTop() + 6, lb - 4, editorTop() + 6 + 18 };
    POINT ptc = { x, y };
    if (PtInRect(&cr, ptc)) {
        closeFolder();
        return;
    }
    // 头部以下才是树区（滚动条与行点击均不响应头部区域）
    int sbTop = editorTop() + SIDEBAR_HEAD_H;
    if (y < sbTop) return;
    // 滚动条滑块 / 轨道
    int viewH = r.bottom - sbTop;
    int contentH = (int)g_treeRows.size() * SIDEBAR_ROW_H;
    if (contentH > viewH) {
        int maxScroll = contentH - viewH;
        if (maxScroll < 1) maxScroll = 1;
        int thumbH = max(20, (int)((double)viewH / contentH * viewH));
        int thumbY = sbTop + (int)((double)g_treeScroll / maxScroll * (viewH - thumbH));
        RECT tr = { lb - 11, thumbY, lb - 2, thumbY + thumbH };
        POINT pt = { x, y };
        if (PtInRect(&tr, pt)) {
            g_treeDrag = true;
            g_treeDragGrab = y - thumbY;
            SetCapture(g_hwnd);
            return;
        }
        if (x >= lb - 11 && x <= lb - 2) { // 点击轨道：上/下翻页
            if (y < thumbY)
                g_treeScroll -= viewH;
            else
                g_treeScroll += viewH;
            clampTreeScroll();
            InvalidateRect(g_hwnd, NULL, TRUE);
            return;
        }
    }
    // 行点击
    int row = (y - sbTop + g_treeScroll) / SIDEBAR_ROW_H;
    if (row >= 0 && row < (int)g_treeRows.size()) {
        TreeNode* n = g_treeRows[row];
        if (n->isDir) {
            if (!n->loaded) loadDir(*n);
            n->expanded = !n->expanded;
            rebuildTreeRows();
            clampTreeScroll();
            InvalidateRect(g_hwnd, NULL, TRUE);
        }
        else {
            openInNewTab(n->fullPath);
        }
    }
}
// 侧栏鼠标移动：更新悬停行 / 头部按钮，触发高亮重绘
void sidebarMove(int x, int y) {
    int lb = leftBar();
    RECT cr = { lb - 22, editorTop() + 6, lb - 4, editorTop() + 6 + 18 };
    POINT pt = { x, y };
    bool onClose = PtInRect(&cr, pt) != 0;
    int row = -1;
    if (!onClose) {
        int sbTop = editorTop() + SIDEBAR_HEAD_H;
        row = (y - sbTop + g_treeScroll) / SIDEBAR_ROW_H;
        if (row < 0 || row >= (int)g_treeRows.size()) row = -1;
    }
    if ((onClose ? (1) : (0)) != g_sideHoverBtn || row != g_sidebarHover) {
        g_sideHoverBtn = onClose ? 1 : 0;
        g_sidebarHover = row;
        InvalidateRect(g_hwnd, NULL, TRUE);
    }
}
// 绘制整个侧栏（背景 / 头部 / 树行 / 分隔线 / 滚动条）
// —— 侧栏文件类型着色辅助 ——
// 取小写扩展名（不含点）；无扩展名返回空串
std::wstring fileExtLower(const std::wstring& name) {
    size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos || dot + 1 >= name.size()) return L"";
    std::wstring e = name.substr(dot + 1);
    for (auto& c : e) c = (wchar_t)towlower(c);
    return e;
}
// 扩展名 → 语言类型（仅映射本软件支持的语言；其余返回 L_TXT）
Lang extToLang(const std::wstring& ext) {
    if (ext == L"cs") return L_CS;
    if (ext == L"sql") return L_SQL;
    if (ext == L"html" || ext == L"htm") return L_HTML;
    if (ext == L"js" || ext == L"mjs" || ext == L"cjs") return L_JS;
    if (ext == L"json") return L_JSON;
    if (ext == L"py" || ext == L"pyw") return L_PY;
    if (ext == L"css") return L_CSS;
    if (ext == L"c" || ext == L"h") return L_C;
    if (ext == L"cpp" || ext == L"cc" || ext == L"cxx" || ext == L"hpp" || ext == L"hxx" ||
        ext == L"hh")
        return L_CPP;
    if (ext == L"java") return L_JAVA;
    if (ext == L"aspx" || ext == L"asax" || ext == L"ascx" || ext == L"ashx" || ext == L"asmx" ||
        ext == L"master")
        return L_ASPX;
    if (ext == L"xml" || ext == L"xaml" || ext == L"svg" || ext == L"config" || ext == L"csproj" ||
        ext == L"vcxproj" || ext == L"resx")
        return L_XML;
    return L_TXT;
}
// 语言类型 → 侧栏文件图标/文字颜色（深浅主题各一套）。未知/文本用默认灰。
COLORREF fileAccent(Lang lg, bool dark) {
    switch (lg) {
    case L_CS:
        return dark ? RGB(230, 180, 34) : RGB(176, 122, 40); // C# 金
    case L_SQL:
        return dark ? RGB(122, 192, 229) : RGB(45, 125, 210); // SQL 蓝
    case L_HTML:
        return dark ? RGB(232, 118, 74) : RGB(214, 69, 43); // HTML 红橙
    case L_JS:
        return dark ? RGB(240, 199, 58) : RGB(180, 150, 20); // JS 黄
    case L_JSON:
        return dark ? RGB(224, 179, 65) : RGB(166, 124, 0); // JSON 琥珀
    case L_PY:
        return dark ? RGB(111, 179, 224) : RGB(45, 110, 175); // Python 蓝
    case L_CSS:
        return dark ? RGB(180, 140, 230) : RGB(140, 90, 200); // CSS 紫
    case L_C:
    case L_CPP:
        return dark ? RGB(130, 165, 230) : RGB(110, 140, 210); // C/C++ 蓝
    case L_JAVA:
        return dark ? RGB(230, 160, 70) : RGB(200, 120, 30); // Java 橙
    case L_ASPX:
        return dark ? RGB(170, 140, 230) : RGB(120, 90, 200); // ASPX 紫
    case L_XML:
        return dark ? RGB(155, 197, 90) : RGB(110, 150, 60); // XML 绿
    default:
        return dark ? RGB(157, 165, 180) : RGB(90, 90, 90); // 文本/未知：默认灰
    }
}

void drawSidebar(HDC mem, const RECT& rc) {
    if (!g_folderOpen) return;
    if (!g_sideFont) ensureSideFont();                  // 防御：确保侧栏字体已就绪
    HGDIOBJ oldSideFnt = SelectObject(mem, g_sideFont); // 侧栏用独立字体，不受编辑器字号影响
    int lb = leftBar();
    int eTop = editorTop();
    // 背景（从菜单栏+标签栏下沿起，覆盖查找条左侧的留白带，避免缝隙）
    COLORREF sbBg = TH.sbBg;
    RECT sbrc = { 0, MENU_H + TAB_H, lb, rc.bottom };
    HBRUSH bb = CreateSolidBrush(sbBg);
    FillRect(mem, &sbrc, bb);
    DeleteObject(bb);
    // 头部
    RECT hdr = { 0, eTop, lb, eTop + SIDEBAR_HEAD_H };
    HBRUSH hb = CreateSolidBrush(TH.sbHeaderBg);
    FillRect(mem, &hdr, hb);
    DeleteObject(hb);
    SetBkMode(mem, TRANSPARENT);
    std::wstring fn = g_folderPath.substr(g_folderPath.find_last_of(L'\\') + 1);
    if (fn.empty()) fn = g_folderPath; // 驱动器根目录时兜底显示完整路径
    RECT tr = { 8, eTop, lb - 24, eTop + SIDEBAR_HEAD_H };
    SetTextColor(mem, TH.sbText);
    DrawText(mem, fn.c_str(), (int)fn.size(), &tr,
        DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    RECT cr = { lb - 22, eTop + 6, lb - 4, eTop + 6 + 18 };
    SetTextColor(mem, (g_sideHoverBtn == 1) ? TH.sbBtnTextHover : TH.sbBtnText);
    DrawText(mem, L"×", 1, &cr, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    // 树行
    int sbTop = eTop + SIDEBAR_HEAD_H;
    COLORREF folderCol = TH.sbFolderCol; // 文件夹统一用文件夹蓝
    for (size_t i = 0; i < g_treeRows.size(); i++) {
        int y = sbTop + (int)i * SIDEBAR_ROW_H - g_treeScroll;
        if (y + SIDEBAR_ROW_H < sbTop) continue;
        if (y > rc.bottom) break;
        TreeNode* n = g_treeRows[i];
        bool active = (n->fullPath == g_filePath);
        bool hover = (g_sidebarHover == (int)i);
        if (active || hover) {
            RECT rr = { 0, y, lb, y + SIDEBAR_ROW_H };
            HBRUSH hb2 = CreateSolidBrush(active ? TH.sbActiveBg : TH.sbHoverBg);
            FillRect(mem, &rr, hb2);
            DeleteObject(hb2);
        }
        int indent = 10 + n->depth * SIDEBAR_INDENT;
        COLORREF col;
        if (n->isDir) {
            col = folderCol;
            drawTreeTri(mem, indent, y + SIDEBAR_ROW_H / 2, n->expanded, TH.sbBtnText);
            drawFolderIcon(mem, indent + 14, (y + (SIDEBAR_ROW_H - 12) / 2), col);
        }
        else {
            col = fileAccent(extToLang(fileExtLower(n->name)), g_dark); // 按文件类型着色
            drawFileIcon(mem, indent + 14, (y + (SIDEBAR_ROW_H - 13) / 2), col);
        }
        RECT nr = { indent + 14 + 16, y, lb - 4, y + SIDEBAR_ROW_H };
        COLORREF txtCol = active || hover
            ? TH.sbText
            : (n->isDir ? TH.sbBtnText : col); // 文件名为类型色，目录为中性灰
        SetTextColor(mem, txtCol);
        DrawText(mem, n->name.c_str(), (int)n->name.size(), &nr,
            DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    if (g_treeRows.empty()) {
        RECT er = { 8, sbTop + 6, lb - 8, sbTop + 24 };
        SetTextColor(mem, TH.sbEmptyText);
        DrawText(mem, L"（空文件夹）", -1, &er, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
    }
    // 分隔线（侧栏与编辑器之间）
    HPEN sp = CreatePen(PS_SOLID, 1, TH.sbDivider);
    HPEN op = (HPEN)SelectObject(mem, sp);
    MoveToEx(mem, lb, MENU_H + TAB_H, NULL);
    LineTo(mem, lb, rc.bottom);
    SelectObject(mem, op);
    DeleteObject(sp);
    // 树滚动条滑块
    int viewH = rc.bottom - sbTop;
    int contentH = (int)g_treeRows.size() * SIDEBAR_ROW_H;
    if (contentH > viewH) {
        int maxScroll = contentH - viewH;
        if (maxScroll < 1) maxScroll = 1;
        int thumbH = max(20, (int)((double)viewH / contentH * viewH));
        int thumbY = sbTop + (int)((double)g_treeScroll / maxScroll * (viewH - thumbH));
        RECT thr = { lb - 11, thumbY, lb - 2, thumbY + thumbH };
        HBRUSH tb = CreateSolidBrush(TH.sbScrollbar);
        FillRect(mem, &thr, tb);
        DeleteObject(tb);
    }
    SelectObject(mem, oldSideFnt); // 还原缓冲区原字体（主编辑框用 g_hFont）
}

// 取标签 i 的显示标题（文件名最后一段，未命名则“未命名”）
std::wstring tabTitle(int i) {
    const std::wstring& p = g_docs[i].filePath;
    if (p.empty()) return L"未命名";
    std::wstring t = p.substr(p.find_last_of(L'\\') + 1);
    return t.empty() ? L"未命名" : t;
}
// 按标题文本宽度动态计算标签宽度（含关闭按钮与内边距），限制在 [TAB_W_MIN,TAB_W_MAX]
int tabWidthFor(int i) {
    std::wstring t = tabTitle(i);
    HDC hdc = GetDC(g_hwnd);
    HGDIOBJ of = SelectObject(hdc, g_hFont);
    SIZE sz;
    GetTextExtentPoint32(hdc, t.c_str(), (int)t.size(), &sz);
    SelectObject(hdc, of);
    ReleaseDC(g_hwnd, hdc);
    int w = sz.cx + TAB_PAD + TAB_CLOSE_W; // 左6 + 右6 + 关闭按钮18 = 30
    if (w < TAB_W_MIN) w = TAB_W_MIN;
    if (w > TAB_W_MAX) w = TAB_W_MAX;
    return w;
}
// 夹紧标签栏水平滚动范围（总宽超过可视区才允许滚动）
void clampTabScroll() {
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    int stripRight = rc.right - PLUS_W;
    if (stripRight < 0) stripRight = 0;
    int total = 0;
    for (int i = 0; i < (int)g_docs.size(); i++) total += tabWidthFor(i);
    int maxS = total > stripRight ? total - stripRight : 0;
    if (g_tabScroll < 0) g_tabScroll = 0;
    if (g_tabScroll > maxS) g_tabScroll = maxS;
}
// 切换/打开/关闭标签后，把当前激活标签滚入可视区
void ensureActiveTabVisible() {
    int n = (int)g_docs.size();
    if (g_active < 0 || g_active >= n) return;
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    int stripRight = rc.right - PLUS_W;
    if (stripRight < 0) stripRight = 0;
    int cx = TAB_X0 - g_tabScroll, ax = 0, aw = 0;
    for (int i = 0; i < n; i++) {
        int w = tabWidthFor(i);
        if (i == g_active) {
            ax = cx;
            aw = w;
        }
        cx += w;
    }
    if (ax < 0)
        g_tabScroll += ax; // 左缘在可视区左侧外
    else if (ax + aw > stripRight)
        g_tabScroll += (ax + aw - stripRight); // 右缘在可视区右侧外
    clampTabScroll();
}

// 绘制顶部标签栏（各文档标签按名称动态宽度 + 新建按钮钉在最右），位于菜单栏下方。
void drawTabBar(HDC mem, const RECT& rc) {
    clampTabScroll(); // 窗口变宽后把标签滚回可视区，避免标签“卡”在滚动偏移处
    COLORREF stripBg = TH.tabStripBg;
    HBRUSH sb = CreateSolidBrush(stripBg);
    FillRect(mem, &rc, sb);
    DeleteObject(sb);
    int top = rc.top;
    int bot = rc.bottom;
    // 标签栏底部分隔线（贯穿到加号按钮左侧）
    int stripRight = rc.right - PLUS_W;
    if (stripRight < 0) stripRight = 0;
    HPEN bp = CreatePen(PS_SOLID, 1, TH.tabDivider);
    HPEN op = (HPEN)SelectObject(mem, bp);
    MoveToEx(mem, 0, bot - 1, NULL);
    LineTo(mem, stripRight, bot - 1);
    SelectObject(mem, op);
    DeleteObject(bp);

    int n = (int)g_docs.size();
    // 裁剪到“标签条带”区域（加号按钮左侧），被滚出左侧的标签不绘制
    int saved = SaveDC(mem);
    IntersectClipRect(mem, 0, top, stripRight, bot);
    SetBkMode(mem, TRANSPARENT);
    int cx = TAB_X0 - g_tabScroll;
    for (int i = 0; i < n; i++) {
        int w = tabWidthFor(i);
        if (cx + w <= 0) {
            cx += w;
            continue;
        }                            // 完全在可视区左侧外，跳过
        if (cx >= stripRight) break; // 已超出条带右界，后续不再可见
        RECT tr = { cx, top, cx + w, bot };
        bool act = (i == g_active);
        COLORREF tb = act ? TH.tabBgActive : TH.tabBg; // 激活标签背景更亮
        HBRUSH tbk = CreateSolidBrush(tb);
        FillRect(mem, &tr, tbk);
        DeleteObject(tbk);
        if (act) {
            // 激活标签底部画一条强调色横线
            HPEN ap = CreatePen(PS_SOLID, 2, TH.tabAccent);
            HPEN ao = (HPEN)SelectObject(mem, ap);
            MoveToEx(mem, cx, bot - 1, NULL);
            LineTo(mem, cx + w, bot - 1);
            SelectObject(mem, ao);
            DeleteObject(ap);
        }
        std::wstring t = tabTitle(i);
        RECT tr2 = { cx + 6, top, cx + w - TAB_CLOSE_W - 6, bot };
        SetTextColor(mem, act ? TH.tabTextActive : TH.tabText);
        DrawText(mem, t.c_str(), (int)t.size(), &tr2,
            DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        // 关闭按钮 ×
        RECT cr = { cx + w - TAB_CLOSE_W, top + 4, cx + w - 4, bot - 4 };
        SetTextColor(mem, TH.tabText);
        DrawText(mem, L"×", 1, &cr, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        cx += w;
    }
    RestoreDC(mem, saved);

    // “新建标签”按钮（+）钉在最右侧，始终可见
    RECT pr = { rc.right - PLUS_W, top + 2, rc.right - 2, bot - 2 };
    HBRUSH pb = CreateSolidBrush(TH.plusBg);
    FillRect(mem, &pr, pb);
    DeleteObject(pb);
    SetBkMode(mem, TRANSPARENT);
    SetTextColor(mem, TH.plusText);
    DrawText(mem, L"+", 1, &pr, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    SetBkMode(mem, OPAQUE);
}

// 主绘制函数：双缓冲（先画到内存 DC，再 BitBlt 到屏幕），避免闪烁。
// ----------------------------------------------------------------------------
// 行渲染缓存：把可见行（非选中态）的文本+语法着色先渲染进离屏位图，
// 之后每帧（尤其拖选/滚动）直接 BitBlt，只在选中/匹配/标记的行上追加覆盖层。
// 这样把“每帧对全屏可见行反复逐段 ExtTextOut 做语法高亮”降到约“可见行数”次 BitBlt。
// ----------------------------------------------------------------------------
void invalidateLineCache() {
    for (auto& kv : g_lineBmp) {
        if (kv.second.dc) DeleteDC(kv.second.dc);
        if (kv.second.bmp) DeleteObject(kv.second.bmp);
    }
    g_lineBmp.clear();
    g_renderVer++;
}

// 把视觉行 v 的“非选中”文本与语法着色渲染到 hdc 的 (0,0) 处（位图局部坐标）。
void renderLineBase(HDC hdc, int v) {
    const Visual& vis = g_visual[v];
    int line = vis.line;
    int base = g_lineStart[line] + vis.col;
    int len = vis.len;
    int w = runPx(base, len);
    if (w < 1) w = 1;
    COLORREF bg = bgColor();
    SetBkMode(hdc, OPAQUE);
    SetBkColor(hdc, bg);
    HBRUSH bgBr = CreateSolidBrush(bg);
    RECT br = { 0, 0, w, g_lineH };
    FillRect(hdc, &br, bgBr);
    DeleteObject(bgBr);
    COLORREF* pal = TH.c;
    std::vector<unsigned char> ttype(len, T_TEXT), tcol(len, 0);
    if (g_langId != L_TXT) {
        const std::vector<Token>& toks = lineTokens(line);
        for (const Token& t : toks) {
            int c0 = t.start, c1 = t.start + t.len;
            int lo = max(c0, vis.col), hi = min(c1, vis.col + len);
            for (int c = lo; c < hi; c++) {
                ttype[c - vis.col] = t.type;
                tcol[c - vis.col] = t.col;
            }
        }
    }
    else {
        int depth = g_lineDepth[line];
        for (int k = 0; k < len; k++) {
            wchar_t c = g_text[base + k];
            if (c == L'(' || c == L'[' || c == L'{') {
                ttype[k] = T_BRACKET;
                tcol[k] = (unsigned char)(depth % 6);
                depth++;
            }
            else if (c == L')' || c == L']' || c == L'}') {
                int d = depth > 0 ? depth - 1 : 0;
                ttype[k] = T_BRACKET;
                tcol[k] = (unsigned char)(d % 6);
                if (depth > 0) depth--;
            }
        }
    }
    int x = 0;
    int k = 0;
    while (k < len) {
        unsigned char ty = ttype[k], tc = tcol[k];
        int j = k + 1;
        while (j < len && ttype[j] == ty && tcol[j] == tc) j++;
        int segLen = j - k;
        COLORREF fg = (ty == T_BRACKET) ? TH.rb[tc] : pal[ty];
        SetTextColor(hdc, fg);
        RECT clip = { 0, 0, w, g_lineH };
        ExtTextOut(hdc, x, 0, ETO_CLIPPED, &clip, g_text.c_str() + base + k, segLen, NULL);
        x += runPx(base + k, segLen);
        k = j;
    }
}

// 超长行（位图超过 LINE_BMP_CAP）回退：沿用原始逐段绘制逻辑直接画到目标 DC。
void drawLineDirect(HDC hdc, int v, int x, int y, int clipR) {
    const Visual& vis = g_visual[v];
    int line = vis.line;
    int base = g_lineStart[line] + vis.col;
    int len = vis.len;
    COLORREF* pal = TH.c;
    COLORREF bg = bgColor();
    const std::vector<Token>& toks = (g_langId == L_TXT) ? g_emptyToks : lineTokens(line);
    std::vector<unsigned char> ttype(len, T_TEXT), tcol(len, 0);
    if (g_langId != L_TXT) {
        for (const Token& t : toks) {
            int c0 = t.start, c1 = t.start + t.len;
            int lo = max(c0, vis.col), hi = min(c1, vis.col + len);
            for (int c = lo; c < hi; c++) {
                ttype[c - vis.col] = t.type;
                tcol[c - vis.col] = t.col;
            }
        }
    }
    else {
        int depth = g_lineDepth[line];
        for (int k = 0; k < len; k++) {
            wchar_t c = g_text[base + k];
            if (c == L'(' || c == L'[' || c == L'{') {
                ttype[k] = T_BRACKET;
                tcol[k] = (unsigned char)(depth % 6);
                depth++;
            }
            else if (c == L')' || c == L']' || c == L'}') {
                int d = depth > 0 ? depth - 1 : 0;
                ttype[k] = T_BRACKET;
                tcol[k] = (unsigned char)(d % 6);
                if (depth > 0) depth--;
            }
        }
    }
    RECT lineClip = { leftBar() + g_gutterW, y, clipR, y + g_lineH };
    int k = 0;
    while (k < len) {
        unsigned char ty = ttype[k], tc = tcol[k];
        bool sel = (g_selStart >= 0 && (base + k) >= g_selStart && (base + k) < g_selEnd);
        bool mt = ((g_matchA >= 0 && (base + k) >= g_matchA && (base + k) < g_matchA + g_matchAw) ||
            (g_matchB >= 0 && (base + k) >= g_matchB && (base + k) < g_matchB + g_matchBw));
        bool mk = (base + k < (int)g_markFlag.size()) ? (g_markFlag[base + k] != 0) : false;
        int j = k + 1;
        while (j < len) {
            bool sel2 = (g_selStart >= 0 && (base + j) >= g_selStart && (base + j) < g_selEnd);
            bool mt2 = ((g_matchA >= 0 && (base + j) >= g_matchA && (base + j) < g_matchA + g_matchAw) ||
                (g_matchB >= 0 && (base + j) >= g_matchB && (base + j) < g_matchB + g_matchBw));
            bool mk2 = (base + j < (int)g_markFlag.size()) ? (g_markFlag[base + j] != 0) : false;
            if (sel2 != sel || mt2 != mt || mk2 != mk || ttype[j] != ty || tcol[j] != tc) break;
            j++;
        }
        int segLen = j - k;
        COLORREF fg = (ty == T_BRACKET) ? TH.rb[tc] : pal[ty];
        COLORREF bk = bg;
        if (mt)
            bk = matchBg();
        else if (sel)
            bk = selBg();
        else if (mk)
            bk = markBg();
        int runW = runPx(base + k, segLen);
        if (sel || mt) {
            RECT segR = { x - 1, y, x + runW + 1, y + g_lineH };
            HBRUSH hb = CreateSolidBrush(bk);
            FillRect(hdc, &segR, hb);
            DeleteObject(hb);
        }
        SetTextColor(hdc, sel ? TH.selText : fg);
        SetBkColor(hdc, bk);
        ExtTextOut(hdc, x, y, ETO_CLIPPED, &lineClip, g_text.c_str() + base + k, segLen, NULL);
        x += runW;
        k = j;
    }
}

// 仅对落在选中/匹配/标记范围内的行画覆盖层（其余行直接 BitBlt 缓存位图即可）。
void drawLineOverlay(HDC hdc, int v, int x, int y, int clipR) {
    const Visual& vis = g_visual[v];
    int line = vis.line;
    int base = g_lineStart[line] + vis.col;
    int len = vis.len;
    COLORREF* pal = TH.c;
    COLORREF bg = bgColor();
    std::vector<unsigned char> ttype(len, T_TEXT), tcol(len, 0);
    if (g_langId != L_TXT) {
        const std::vector<Token>& toks = lineTokens(line);
        for (const Token& t : toks) {
            int c0 = t.start, c1 = t.start + t.len;
            int lo = max(c0, vis.col), hi = min(c1, vis.col + len);
            for (int c = lo; c < hi; c++) {
                ttype[c - vis.col] = t.type;
                tcol[c - vis.col] = t.col;
            }
        }
    }
    else {
        int depth = g_lineDepth[line];
        for (int k = 0; k < len; k++) {
            wchar_t c = g_text[base + k];
            if (c == L'(' || c == L'[' || c == L'{') {
                ttype[k] = T_BRACKET;
                tcol[k] = (unsigned char)(depth % 6);
                depth++;
            }
            else if (c == L')' || c == L']' || c == L'}') {
                int d = depth > 0 ? depth - 1 : 0;
                ttype[k] = T_BRACKET;
                tcol[k] = (unsigned char)(d % 6);
                if (depth > 0) depth--;
            }
        }
    }
    SetBkMode(hdc, OPAQUE);
    int k = 0;
    while (k < len) {
        bool sel = (g_selStart >= 0 && (base + k) >= g_selStart && (base + k) < g_selEnd);
        bool mt = ((g_matchA >= 0 && (base + k) >= g_matchA && (base + k) < g_matchA + g_matchAw) ||
            (g_matchB >= 0 && (base + k) >= g_matchB && (base + k) < g_matchB + g_matchBw));
        bool mk = (base + k < (int)g_markFlag.size()) ? (g_markFlag[base + k] != 0) : false;
        int j = k + 1;
        while (j < len) {
            bool sel2 = (g_selStart >= 0 && (base + j) >= g_selStart && (base + j) < g_selEnd);
            bool mt2 = ((g_matchA >= 0 && (base + j) >= g_matchA && (base + j) < g_matchA + g_matchAw) ||
                (g_matchB >= 0 && (base + j) >= g_matchB && (base + j) < g_matchB + g_matchBw));
            bool mk2 = (base + j < (int)g_markFlag.size()) ? (g_markFlag[base + j] != 0) : false;
            if (sel2 != sel || mt2 != mt || mk2 != mk) break;
            j++;
        }
        int segLen = j - k;
        int runW = runPx(base + k, segLen);
        COLORREF bk = bg;
        if (mt)
            bk = matchBg();
        else if (sel)
            bk = selBg();
        else if (mk)
            bk = markBg();
        if (bk != bg) {
            RECT segR = { x - 1, y, x + runW + 1, y + g_lineH };
            HBRUSH hb = CreateSolidBrush(bk);
            FillRect(hdc, &segR, hb);
            DeleteObject(hb);
        }
        if (sel) {
            SetTextColor(hdc, TH.selText);
            SetBkColor(hdc, bk);
            RECT lc = { leftBar() + g_gutterW, y, clipR, y + g_lineH };
            ExtTextOut(hdc, x, y, ETO_CLIPPED, &lc, g_text.c_str() + base + k, segLen, NULL);
        }
        else if (mt || mk) {
            COLORREF fg = (ttype[k] == T_BRACKET) ? TH.rb[tcol[k]] : pal[ttype[k]];
            SetTextColor(hdc, fg);
            SetBkColor(hdc, bk);
            RECT lc = { leftBar() + g_gutterW, y, clipR, y + g_lineH };
            ExtTextOut(hdc, x, y, ETO_CLIPPED, &lc, g_text.c_str() + base + k, segLen, NULL);
        }
        x += runW;
        k = j;
    }
}

bool lineNeedsOverlay(int base, int len) {
    if (g_selStart >= 0 && base < g_selEnd && base + len > g_selStart) return true;
    if (g_matchA >= 0 && base < g_matchA + g_matchAw && base + len > g_matchA) return true;
    if (g_matchB >= 0 && base < g_matchB + g_matchBw && base + len > g_matchB) return true;
    if (!g_markFlag.empty()) {
        int e = base + len;
        if (e > (int)g_markFlag.size()) e = (int)g_markFlag.size();
        for (int c = base; c < e; c++)
            if (g_markFlag[c]) return true;
    }
    return false;
}

LineBmp& getLineBmp(int v) {
    auto it = g_lineBmp.find(v);
    if (it != g_lineBmp.end()) {
        if (it->second.ver == g_renderVer && it->second.cached) return it->second;
        if (it->second.dc) DeleteDC(it->second.dc);
        if (it->second.bmp) DeleteObject(it->second.bmp);
        g_lineBmp.erase(it);
    }
    const Visual& vis = g_visual[v];
    int line = vis.line, base = g_lineStart[line] + vis.col, len = vis.len;
    int w = runPx(base, len);
    if (w < 1) w = 1;
    LineBmp lb;
    lb.ver = g_renderVer;
    lb.cached = false;
    lb.w = 0;
    lb.dc = NULL;
    lb.bmp = NULL;
    if (w <= LINE_BMP_CAP) {
        if (!g_hdcScreen) g_hdcScreen = GetDC(g_hwnd);
        lb.dc = CreateCompatibleDC(g_hdcScreen);
        if (lb.dc) {
            lb.bmp = CreateCompatibleBitmap(g_hdcScreen, w, g_lineH);
            if (lb.bmp) {
                HBITMAP oldBmp = (HBITMAP)SelectObject(lb.dc, lb.bmp);
                SelectObject(lb.dc, g_hFont);
                renderLineBase(lb.dc, v);
                if (oldBmp) DeleteObject(oldBmp); // 释放兼容 DC 自带的 1x1 默认位图
                lb.w = w;
                lb.cached = true;
            }
            else {
                DeleteDC(lb.dc);
                lb.dc = NULL;
            }
        }
    }
    g_lineBmp[v] = lb;
    if ((int)g_lineBmp.size() > LINE_BMP_MAX) {
        auto itb = g_lineBmp.begin();
        if (itb->first != v) { // 不删除刚插入的自身
            if (itb->second.dc) DeleteDC(itb->second.dc);
            if (itb->second.bmp) DeleteObject(itb->second.bmp);
            g_lineBmp.erase(itb);
        }
    }
    return g_lineBmp[v];
}

// ---- 增量重绘辅助：滚动像素平移 + 脏矩形，避免整屏重绘（滚动/拖选卡顿优化）----
static HDC g_bufDC = NULL;      // 持久离屏缓冲 DC（避免每帧分配全屏位图）
static HBITMAP g_bufBmp = NULL; // 持久离屏缓冲位图
static int g_bufW = 0, g_bufH = 0;
static void ensureBuf(int w, int h) {
    if (g_bufDC && w == g_bufW && h == g_bufH) return;
    if (g_bufDC) {
        DeleteDC(g_bufDC);
        if (g_bufBmp) DeleteObject(g_bufBmp);
        g_bufDC = NULL;
        g_bufBmp = NULL;
    }
    HDC scr = GetDC(g_hwnd);
    g_bufDC = CreateCompatibleDC(scr);
    g_bufBmp = CreateCompatibleBitmap(scr, w, h);
    ReleaseDC(g_hwnd, scr);
    SelectObject(g_bufDC, g_bufBmp);
    g_bufW = w;
    g_bufH = h;
}
static bool rectsIntersect(const RECT& a, const RECT& b) {
    return a.left < b.right && a.right > b.left && a.top < b.bottom && a.bottom > b.top;
}
// 视觉行 v 与偏移区间 [s,s+w) 的交集（返回该行内列范围 [c0,c1)）；不相交返回 false
static bool rowRun(int v, int s, int w, int& c0, int& c1) {
    if (s < 0 || w <= 0) return false;
    const Visual& vis = g_visual[v];
    int rs = g_lineStart[vis.line] + vis.col;
    int re = rs + vis.len;
    int a = max(s, rs), b = min(s + w, re);
    if (a >= b) return false;
    c0 = a - rs;
    c1 = b - rs;
    return true;
}
// 视觉行 v 在区间 [s,s+w) 与 [ns,ns+nw) 下的高亮段是否不同（含行内区段变化）
static bool runChanged(int v, int s, int w, int ns, int nw) {
    int a0, a1, b0, b1;
    bool ow = rowRun(v, s, w, a0, a1);
    bool nw2 = rowRun(v, ns, nw, b0, b1);
    if (ow != nw2) return true;
    if (ow && (a0 != b0 || a1 != b1)) return true;
    return false;
}
// 仅重绘“选中/匹配状态变化”的可见行（拖选卡顿优化核心）
static void repaintSelDir(int oS, int oE, int nS, int nE, int oMa, int oMaW, int oMb, int oMbW,
    int nMa, int nMaW, int nMb, int nMbW) {
    RECT r;
    GetClientRect(g_hwnd, &r);
    int eTop = editorTop();
    int visH = (r.bottom - eTop) / g_lineH + 1;
    int sV = g_topLine, eV = g_topLine + visH + 1;
    if (eV > g_visualCount) eV = g_visualCount;
    for (int v = sV; v < eV; v++) {
        bool ch = false;
        if (runChanged(v, oS, oE - oS, nS, nE - nS)) ch = true;
        if (!ch && runChanged(v, oMa, oMaW, nMa, nMaW)) ch = true;
        if (!ch && runChanged(v, oMb, oMbW, nMb, nMbW)) ch = true;
        if (ch) {
            int y = eTop + (v - g_topLine) * g_lineH;
            RECT lr = { leftBar() + g_gutterW, y, r.right, y + g_lineH };
            InvalidateRect(g_hwnd, &lr, TRUE);
        }
    }
}
// 垂直滚动：仅把已有像素平移 delta 行，重绘新露出条带（滚动卡顿优化核心）
static void scrollByLines(int delta) {
    if (delta == 0) return;
    int oldTop = g_topLine;
    g_topLine += delta;
    // 钳制滚动位置：不能为负（否则 paint 会以负数下标访问 g_visual / g_lineStart，
    // 空 vector 的负下标会读到 null-12 = 0xFFFF...F4，触发 0xC0000005）；也不能越过底部。
    if (g_topLine < 0) g_topLine = 0;
    if (g_visualCount > 0 && g_topLine > g_visualCount - 1) g_topLine = g_visualCount - 1;
    int d = g_topLine - oldTop; // 实际发生的位移（到顶/到底时为 0，避免错位滚动）
    if (d == 0) {
        updateScroll();
        updateCaretPos();
        return;
    }
    int dy = -d * g_lineH;
    RECT r;
    GetClientRect(g_hwnd, &r);
    int eTop = editorTop();
    RECT sr = { leftBar(), eTop, r.right, r.bottom };
    ScrollWindowEx(g_hwnd, 0, dy, &sr, &sr, NULL, NULL, SW_INVALIDATE);
    updateScroll();
    updateCaretPos();
}
// 查找条绘制（从 paint 中抽取，便于按更新矩形局部重绘）
static void drawFindBar(HDC mem, RECT rc) {
    int by = rc.top, bh = rc.bottom - rc.top;
    RECT fbr = { leftBar(), by, rc.right, by + bh };
    COLORREF fb = TH.findBg;
    HBRUSH fbk = CreateSolidBrush(fb);
    FillRect(mem, &fbr, fbk);
    DeleteObject(fbk);
    HPEN sp = CreatePen(PS_SOLID, 1, TH.findDivider);
    HPEN sop = (HPEN)SelectObject(mem, sp);
    MoveToEx(mem, 0, by + bh - 1, NULL);
    LineTo(mem, rc.right, by + bh - 1);
    SelectObject(mem, sop);
    DeleteObject(sp);
    int mx = leftBar() + g_gutterW + 10, my = by + bh / 2, rad = 6;
    HPEN ip = CreatePen(PS_SOLID, 2, TH.findIcon);
    HPEN iop = (HPEN)SelectObject(mem, ip);
    HBRUSH ib = (HBRUSH)GetStockObject(NULL_BRUSH);
    HBRUSH ibo = (HBRUSH)SelectObject(mem, ib);
    Ellipse(mem, mx - rad, my - rad, mx + rad, my + rad);
    MoveToEx(mem, mx + rad - 2, my + rad - 2, NULL);
    LineTo(mem, mx + rad + 3, my + rad + 3);
    SelectObject(mem, iop);
    DeleteObject(ip);
    SelectObject(mem, ibo);
    drawFindButton(mem, g_rPrev, L"上一项", g_findHover == 1, g_findPress == 1);
    drawFindButton(mem, g_rNext, L"下一项", g_findHover == 2, g_findPress == 2);
    drawFindCloseBtn(mem, g_rClose, g_findHover == 3, g_findPress == 3);
}

void paint() {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(g_hwnd, &ps);
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) {
        EndPaint(g_hwnd, &ps);
        return;
    }
    int eTop = editorTop(); // 编辑区从标签栏（及可能的查找条）下方开始
    RECT ur = ps.rcPaint;   // 仅本次需要重绘的更新矩形
    ensureBuf(rc.right, rc.bottom);
    HDC mem = g_bufDC;
    HGDIOBJ oldFnt = SelectObject(mem, g_hFont); // 注意：此处返回的“旧对象”实为 g_bufBmp，收尾时归还
    SetBkMode(mem, OPAQUE);

    COLORREF bg = bgColor();
    COLORREF gb = gutterBg();

    // 编辑器文本带：仅重绘与更新矩形相交的部分（滚动/拖选只触及少量行）
    int y0 = max((int)ur.top, eTop), y1 = min((int)ur.bottom, (int)rc.bottom);
    if (y1 > y0) {
        RECT bgr = { 0, y0, rc.right, y1 };
        HBRUSH bgBr = CreateSolidBrush(bg);
        FillRect(mem, &bgr, bgBr);
        DeleteObject(bgBr);

        int startV = (y0 - eTop) / g_lineH + g_topLine;
        int endV = (y1 - eTop - 1) / g_lineH + g_topLine;
        // 防御：可见行范围必须非负且合法，避免以负数下标访问 g_visual（空 vector 负下标
        // 会读到 null-12，导致 0xC0000005 访问冲突）。
        if (startV < 0) startV = 0;
        if (startV < g_topLine) startV = g_topLine;
        if (endV < 0) endV = -1;
        if (endV >= g_visualCount) endV = g_visualCount - 1;
        if (endV < startV) endV = startV - 1; // 空范围，循环不执行
        for (int v = startV; v <= endV; v++) {
            int y = eTop + (v - g_topLine) * g_lineH;
            const Visual& vis = g_visual[v];
            int line = vis.line;
            int base = g_lineStart[line] + vis.col;
            int len = vis.len;
            int x = (g_wrap) ? (leftBar() + g_gutterW)
                : (leftBar() + g_gutterW - g_scrollX + linePrefixPx(line, vis.col));
            LineBmp& lb = getLineBmp(v);
            if (lb.cached)
                BitBlt(mem, x, y, lb.w, g_lineH, lb.dc, 0, 0, SRCCOPY);
            else
                drawLineDirect(mem, v, x, y, rc.right); // 超长行回退到原始逐段绘制
            if (lineNeedsOverlay(base, len)) drawLineOverlay(mem, v, x, y, rc.right);
        }
        // 行号区（覆盖在文本之上，遮挡横向滚动溢出的文本）
        RECT grc = { leftBar(), y0, leftBar() + g_gutterW, y1 };
        HBRUSH gbBr = CreateSolidBrush(gb);
        FillRect(mem, &grc, gbBr);
        DeleteObject(gbBr);
        HPEN pen = CreatePen(PS_SOLID, 1, TH.findDivider);
        HPEN op = (HPEN)SelectObject(mem, pen);
        MoveToEx(mem, leftBar() + g_gutterW, y0, NULL);
        LineTo(mem, leftBar() + g_gutterW, y1);
        SelectObject(mem, op);
        DeleteObject(pen);
        SetTextColor(mem, gutterFg());
        SetBkColor(mem, gb);
        wchar_t num[16];
        for (int v = startV; v <= endV; v++) {
            int y = eTop + (v - g_topLine) * g_lineH;
            int lineNo = v + 1;
            _snwprintf(num, 15, L"%d", lineNo);
            int tw = (int)wcslen(num) * g_charW;
            RECT nr = { leftBar() + g_gutterW - 6 - tw, y, leftBar() + g_gutterW - 6, y + g_lineH };
            ExtTextOut(mem, leftBar() + g_gutterW - 6 - tw, y, ETO_CLIPPED | ETO_OPAQUE, &nr, num,
                (UINT)wcslen(num), NULL);
        }
    }

    // 查找条（仅当更新矩形与之相交时重绘）
    if (g_hFind) {
        RECT fbrc = { leftBar(), MENU_H + TAB_H, rc.right, MENU_H + TAB_H + FIND_H };
        if (rectsIntersect(ur, fbrc)) drawFindBar(mem, fbrc);
    }
    // 标签栏（仅当更新矩形与之相交时重绘）
    {
        RECT tbrc = { 0, MENU_H, rc.right, MENU_H + TAB_H };
        if (rectsIntersect(ur, tbrc)) drawTabBar(mem, tbrc);
    }
    // 顶部自绘菜单栏（必须最后画，覆盖任何可能从上方泄漏的背景）
    {
        RECT mbrc = { 0, 0, rc.right, MENU_H };
        if (rectsIntersect(ur, mbrc)) drawMenuBar(mem, mbrc);
    }
    // 左侧文件夹浏览器（仅当更新矩形与之相交时重绘）
    if (g_folderOpen) {
        RECT sbrc = { 0, eTop, leftBar(), rc.bottom };
        if (rectsIntersect(ur, sbrc)) drawSidebar(mem, rc);
    }

    // 仅把更新矩形区域从内存 DC 拷到屏幕（不再整屏 BitBlt）
    SelectObject(mem, oldFnt);
    if (ur.right > ur.left && ur.bottom > ur.top)
        BitBlt(hdc, ur.left, ur.top, ur.right - ur.left, ur.bottom - ur.top, mem, ur.left, ur.top,
            SRCCOPY);
    EndPaint(g_hwnd, &ps);
}

// ----------------------------------------------------------------------------
// 文件读取（编码识别）
// 支持 UTF-8(BOM/无BOM)、UTF-16 LE(BOM)、UTF-16 BE(BOM)、以及 GBK/系统 ANSI 回退。
// ----------------------------------------------------------------------------
std::wstring decodeBytes(const std::vector<BYTE>& b) {
    // UTF-8 BOM (EF BB BF)
    if (b.size() >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF) {
        g_enc = 1;
        int n = (int)b.size() - 3;
        std::string s((char*)b.data() + 3, n);
        int wn = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
        std::wstring w;
        w.resize(wn);
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], wn);
        return w;
    }
    // UTF-16 LE BOM (FF FE)
    if (b.size() >= 2 && b[0] == 0xFF && b[1] == 0xFE) {
        g_enc = 2;
        std::wstring w((wchar_t*)(b.data() + 2), (b.size() - 2) / 2);
        return w;
    }
    // UTF-16 BE BOM (FE FF)：字节序需翻转
    if (b.size() >= 2 && b[0] == 0xFE && b[1] == 0xFF) {
        g_enc = 3;
        std::wstring w;
        int n = (int)b.size() - 2;
        w.resize(n / 2);
        for (int i = 0; i < n / 2; i++) { w[i] = (wchar_t)(b[2 + 2 * i + 1] << 8 | b[2 + 2 * i]); }
        return w;
    }
    // 无 BOM：先尝试按 UTF-8 解码（严格模式 MB_ERR_INVALID_CHARS）
    int wn =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (char*)b.data(), (int)b.size(), NULL, 0);
    if (wn > 0) {
        g_enc = 0;
        std::wstring w;
        w.resize(wn);
        MultiByteToWideChar(CP_UTF8, 0, (char*)b.data(), (int)b.size(), &w[0], wn);
        return w;
    }
    // 失败则按系统 ANSI 代码页（中文 Windows 通常为 GBK）解码
    g_enc = 4;
    int an = MultiByteToWideChar(CP_ACP, 0, (char*)b.data(), (int)b.size(), NULL, 0);
    std::wstring w;
    w.resize(an);
    MultiByteToWideChar(CP_ACP, 0, (char*)b.data(), (int)b.size(), &w[0], an);
    return w;
}

// 编码回写：将 wstring 按指定编码（g_enc 语义）转回字节，保存时按原编码写盘，保留 BOM。
std::vector<BYTE> encodeBytes(const std::wstring& w, int enc) {
    std::vector<BYTE> out;
    if (enc == 2) { // UTF-16 LE + BOM
        BYTE bom[] = { 0xFF, 0xFE };
        out.insert(out.end(), bom, bom + 2);
        out.insert(out.end(), (BYTE*)w.data(), (BYTE*)(w.data() + w.size()));
    }
    else if (enc == 3) { // UTF-16 BE + BOM
        BYTE bom[] = { 0xFE, 0xFF };
        out.insert(out.end(), bom, bom + 2);
        for (wchar_t c : w) {
            out.push_back((BYTE)(c >> 8));
            out.push_back((BYTE)c);
        }
    }
    else if (enc == 1) { // UTF-8 + BOM
        BYTE bom[] = { 0xEF, 0xBB, 0xBF };
        out.insert(out.end(), bom, bom + 2);
        int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), NULL, 0, NULL, NULL);
        std::string s;
        s.resize(n);
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, NULL, NULL);
        out.insert(out.end(), (BYTE*)s.data(), (BYTE*)s.data() + n);
    }
    else if (enc == 4) { // ANSI / GBK（系统代码页）
        int n = WideCharToMultiByte(CP_ACP, 0, w.c_str(), (int)w.size(), NULL, 0, NULL, NULL);
        std::string s;
        s.resize(n);
        WideCharToMultiByte(CP_ACP, 0, w.c_str(), (int)w.size(), &s[0], n, NULL, NULL);
        out.insert(out.end(), (BYTE*)s.data(), (BYTE*)s.data() + n);
    }
    else { // 0 = UTF-8 无 BOM
        int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), NULL, 0, NULL, NULL);
        std::string s;
        s.resize(n);
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, NULL, NULL);
        out.insert(out.end(), (BYTE*)s.data(), (BYTE*)s.data() + n);
    }
    return out;
}

// 打开文件：读入字节 -> 解码为 wstring -> 重切行 -> 重算视觉行 -> 重置视图 -> 重绘。
void loadFile(const std::wstring& path) {
    HANDLE h = CreateFile(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD sz = GetFileSize(h, NULL);
    if (sz == INVALID_FILE_SIZE || sz > 200 * 1024 * 1024) {
        CloseHandle(h);
        return;
    } // 超过 200MB 拒绝加载
    std::vector<BYTE> buf(sz);
    DWORD rd = 0;
    ReadFile(h, buf.data(), sz, &rd, NULL);
    CloseHandle(h);
    g_filePath = path;
    g_text = decodeBytes(buf);
    g_funcDirty = true;
    g_undoStack.clear();
    g_redoStack.clear(); // 载入新文件：撤销/重做历史作废
    g_markWord.clear();
    g_markFlag.clear();
    g_markRanges.clear(); // 文本变化，清除分词高亮
    g_langId = langFromName();
    rebuildLines();
    buildVisual();
    g_caretOff = 0;
    g_anchorOff = 0;
    g_selStart = -1;
    g_selEnd = -1;
    g_topLine = 0;
    g_scrollX = 0;
    g_dirty = false;
    if (g_hwnd) {
        ensureFont();
        updateScroll();
        InvalidateRect(g_hwnd, NULL, TRUE);
        updateCaretPos();
    }
    setWindowTitle();
}

// 根据当前文件名与语言设置窗口标题。
void setWindowTitle() {
    std::wstring name =
        g_filePath.empty() ? L"LiteReader" : g_filePath.substr(g_filePath.find_last_of(L'\\') + 1);
    std::wstring lang;
    switch (g_langId) {
    case L_CS:
        lang = L" C#";
        break;
    case L_SQL:
        lang = L" SQL";
        break;
    case L_HTML:
        lang = L" HTML";
        break;
    case L_JS:
        lang = L" JS";
        break;
    case L_JSON:
        lang = L" JSON";
        break;
    case L_PY:
        lang = L" Python";
        break;
    case L_CSS:
        lang = L" CSS";
        break;
    case L_C:
        lang = L" C";
        break;
    case L_CPP:
        lang = L" C++";
        break;
    case L_JAVA:
        lang = L" Java";
        break;
    case L_ASPX:
        lang = L" ASPX";
        break;
    case L_XML:
        lang = L" XML";
        break;
    default:
        lang = L" 文本";
    }
    bool dirty = (g_active >= 0 && g_active < (int)g_docs.size()) ? g_docs[g_active].dirty : g_dirty;
    SetWindowText(g_hwnd, (name + L" - LiteReader  ·" + lang + (dirty ? L"  *" : L"")).c_str());
}

// 统一的文本替换入口：把 [start,end) 替换为 ins，并（可选）记录到撤销栈。
void applyEdit(int start, int end, const std::wstring& ins, bool record) {
    if (start < 0) start = 0;
    if (end > (int)g_text.size()) end = (int)g_text.size();
    if (start > end) std::swap(start, end);
    if (record) {
        EditStep s;
        s.start = start;
        s.del = g_text.substr(start, end - start);
        s.ins = ins;
        g_undoStack.push_back(s);
        g_redoStack.clear(); // 任何新编辑都会使“重做”历史失效
    }
    g_text.replace(start, end - start, ins);
    g_caretOff = start + (int)ins.size();
    g_selStart = -1;
    g_selEnd = -1;
    g_anchorOff = g_caretOff;
    g_markWord.clear();
    g_markFlag.clear();
    g_markRanges.clear(); // 文本变化，清除分词高亮
    g_matchA = -1;
    g_matchB = -1;
    g_funcDirty = true; // 文本变化，函数名缓存失效
    rebuildLines();
    buildVisual();
    updateScroll();
    if (g_active >= 0 && g_active < (int)g_docs.size()) g_docs[g_active].dirty = true;
    g_dirty = true;
    updateCaretPos();
    InvalidateRect(g_hwnd, NULL, TRUE);
    setWindowTitle();
}
// 撤销：取最近一次编辑，删除其插入串、填回删除串，并压入重做栈。
void undo() {
    if (g_undoStack.empty()) return;
    EditStep s = g_undoStack.back();
    g_undoStack.pop_back();
    g_text.replace(s.start, s.ins.size(), s.del);
    g_funcDirty = true;
    g_redoStack.push_back(s);
    g_caretOff = s.start + (int)s.del.size();
    g_selStart = -1;
    g_selEnd = -1;
    g_anchorOff = g_caretOff;
    g_markWord.clear();
    g_markFlag.clear();
    g_markRanges.clear();
    g_matchA = -1;
    g_matchB = -1;
    rebuildLines();
    buildVisual();
    updateScroll();
    if (g_active >= 0 && g_active < (int)g_docs.size()) g_docs[g_active].dirty = true;
    g_dirty = true;
    updateCaretPos();
    InvalidateRect(g_hwnd, NULL, TRUE);
    setWindowTitle();
}
// 重做：取最近一次撤销，重新应用其插入串，并压回撤销栈。
void redo() {
    if (g_redoStack.empty()) return;
    EditStep s = g_redoStack.back();
    g_redoStack.pop_back();
    g_text.replace(s.start, s.del.size(), s.ins);
    g_funcDirty = true;
    g_undoStack.push_back(s);
    g_caretOff = s.start + (int)s.ins.size();
    g_selStart = -1;
    g_selEnd = -1;
    g_anchorOff = g_caretOff;
    g_markWord.clear();
    g_markFlag.clear();
    g_markRanges.clear();
    g_matchA = -1;
    g_matchB = -1;
    rebuildLines();
    buildVisual();
    updateScroll();
    if (g_active >= 0 && g_active < (int)g_docs.size()) g_docs[g_active].dirty = true;
    g_dirty = true;
    updateCaretPos();
    InvalidateRect(g_hwnd, NULL, TRUE);
    setWindowTitle();
}
// 轻量编辑：在光标处插入文本（若存在选区则先替换选区）。插入后重置选区、重切行、重绘、置脏标记。
void insertText(const std::wstring& s) {
    int start = g_caretOff, end = g_caretOff;
    if (g_selStart >= 0) {
        start = g_selStart;
        end = g_selEnd;
    }
    applyEdit(start, end, s, true);
}
// 轻量编辑：删除字符。forward=true 删除光标后（Delete 键），false 删除光标前（Backspace
// 键）；有选区则删除选区。
void deleteChar(bool forward) {
    int start = g_caretOff, end = g_caretOff;
    if (g_selStart >= 0) {
        start = g_selStart;
        end = g_selEnd;
    }
    else if (forward) {
        if (g_caretOff < (int)g_text.size())
            end = g_caretOff + 1;
        else
            return;
    }
    else {
        if (g_caretOff > 0)
            start = g_caretOff - 1;
        else
            return;
    }
    if (start >= end) return;
    applyEdit(start, end, L"", true);
}
// 另存为对话框（保存时用）
std::wstring saveFileDialog() {
    OPENFILENAME ofn = { sizeof(ofn) };
    wchar_t buf[MAX_PATH] = { 0 };
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter =
        L"文本/"
        L"代码\0*.txt;*.cs;*.sql;*.html;*.htm;*.xml;*.js;*.json;*.css;*.py;*.pyw\0所有文件\0*.*\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (GetSaveFileName(&ofn)) return std::wstring(buf);
    return L"";
}
// 保存：按原编码写回磁盘；无路径时弹“另存为”。写成功后清除脏标记。
void saveFile() {
    if (g_filePath.empty()) {
        std::wstring p = saveFileDialog();
        if (p.empty()) return;
        g_filePath = p;
    }
    std::vector<BYTE> buf = encodeBytes(g_text, g_enc);
    HANDLE h = CreateFile(g_filePath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD wr = 0;
    WriteFile(h, buf.data(), (DWORD)buf.size(), &wr, NULL);
    CloseHandle(h);
    if (g_active >= 0 && g_active < (int)g_docs.size()) {
        g_docs[g_active].filePath = g_filePath;
        g_docs[g_active].dirty = false;
    }
    g_dirty = false;
    setWindowTitle();
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
void doFind(bool forward); // 前置声明（定义于其后）
void toggleFind();         // 前置声明（定义于其后）
LRESULT CALLBACK FindEditProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_KEYDOWN) {
        if (wp == VK_RETURN) {
            doFind(true);
            SetFocus(g_hFind);
            return 0;
        } // 回车 = 下一个（焦点留在输入框）
        if (wp == VK_ESCAPE) {
            toggleFind();
            return 0;
        } // ESC 关闭查找条
    }
    return CallWindowProc(g_oldFindProc, hw, msg, wp, lp);
}

void toggleFind() {
    if (g_hFind) {
        DestroyWindow(g_hFind);
        g_hFind = NULL;
        g_oldFindProc = NULL;
        g_findHover = 0;
        g_findPress = 0; // 隐藏：销毁输入框与查找条
        InvalidateRect(g_hwnd, NULL, TRUE);
        return;
    }
    int ew = 240, eh = 20, ey = MENU_H + TAB_H + 5;
    int ex = leftBar() + g_gutterW + 28; // 左侧留出放大镜图标位置（随侧栏右移）
    g_hFind =
        CreateWindow(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL | ES_WANTRETURN,
            ex, ey, ew, eh, g_hwnd, (HMENU)2001, g_hInst, NULL);
    // 子类化输入框：捕获回车/ESC 的 WM_KEYDOWN，避免焦点丢失到标题栏
    g_oldFindProc = (WNDPROC)SetWindowLongPtr(g_hFind, GWLP_WNDPROC, (LONG_PTR)FindEditProc);
    // 计算三个自绘按钮的命中矩形（位于输入框右侧）
    int btnY = MENU_H + TAB_H + 4, btnH = 22, btnW = 64;
    int nx = ex + ew + 8;
    g_rPrev = { nx, btnY, nx + btnW, btnY + btnH };
    g_rNext = { nx + btnW + 6, btnY, nx + btnW + 6 + btnW, btnY + btnH };
    int cx = nx + btnW * 2 + 6 + 12;
    g_rClose = { cx, btnY, cx + 24, btnY + btnH };
    // 启用 WM_MOUSELEAVE，便于鼠标离开窗口时清除悬停高亮
    TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, g_hwnd, 0 };
    TrackMouseEvent(&tme);
    SetFocus(g_hFind);
    InvalidateRect(g_hwnd, NULL, TRUE);
}
// 执行查找：forward 为 true 向后找，false 向前找。命中后选中并移动光标。
void doFind(bool forward) {
    if (!g_hFind) return;
    int n = GetWindowTextLength(g_hFind);
    if (n <= 0) return;
    std::wstring q;
    q.resize(n + 1);
    GetWindowText(g_hFind, &q[0], n + 1);
    q.resize(n);
    int start = g_caretOff;
    if (forward) {
        int p = (int)g_text.find(q, start);
        if (p < 0) p = (int)g_text.find(q, 0); // 到末尾没找到则从头再找（循环）
        if (p >= 0) {
            g_selStart = p;
            g_selEnd = p + (int)q.size();
            setCaret(p + (int)q.size());
            g_selStart = p;
            g_selEnd = p + (int)q.size();
        }
    }
    else {
        int p = (int)g_text.rfind(q, start - (int)q.size() - 1);
        if (p < 0) p = (int)g_text.rfind(q, g_text.size()); // 向前没找到则从尾部向前找（循环）
        if (p >= 0) {
            g_selStart = p;
            g_selEnd = p + (int)q.size();
            setCaret(p);
            g_selStart = p;
            g_selEnd = p + (int)q.size();
        }
    }
}

// ----------------------------------------------------------------------------
// 文件关联注册（HKCU，无需管理员）
// 把本程序写入“打开方式”列表，并注册为 .txt/.cs 等扩展名的候选打开程序。
// ----------------------------------------------------------------------------
void registerDefault() {
    std::wstring exePath(MAX_PATH, 0);
    GetModuleFileName(NULL, &exePath[0], MAX_PATH);
    exePath.resize(wcslen(exePath.c_str()));
    HKEY hk;
    std::wstring base = L"Software\\Classes\\Applications\\LiteReader.exe";
    if (RegCreateKeyEx(HKEY_CURRENT_USER, base.c_str(), 0, NULL, 0, KEY_WRITE, NULL, &hk, NULL) ==
        ERROR_SUCCESS) {
        RegSetValueEx(hk, L"FriendlyAppName", 0, REG_SZ, (BYTE*)L"LiteReader 代码阅读器",
            (DWORD)(wcslen(L"LiteReader 代码阅读器") + 1) * 2);
        HKEY hs;
        if (RegCreateKeyEx(hk, L"shell\\open\\command", 0, NULL, 0, KEY_WRITE, NULL, &hs, NULL) ==
            ERROR_SUCCESS) {
            std::wstring cmd =
                L"\"" + exePath + L"\" \"%1\""; // 双击文件时用本程序打开，并把路径作为 %1 传入
            RegSetValueEx(hs, L"", 0, REG_SZ, (BYTE*)cmd.c_str(), (DWORD)(wcslen(cmd.c_str()) + 1) * 2);
            RegCloseKey(hs);
        }
        RegCloseKey(hk);
    }
    const wchar_t* exts[] = { L".txt", L".cs",   L".sql", L".html", L".htm",  L".xml",
                             L".js",  L".json", L".css", L".py",   L".pyw",  L".c",
                             L".h",   L".cpp",  L".hpp", L".java", L".aspx", NULL };
    for (int i = 0; exts[i]; i++) {
        std::wstring ek =
            L"Software\\Classes\\" + std::wstring(exts[i]) + L"\\OpenWithList\\LiteReader.exe";
        HKEY h2;
        RegCreateKeyEx(HKEY_CURRENT_USER, ek.c_str(), 0, NULL, 0, KEY_WRITE, NULL, &h2, NULL);
        if (h2) { RegCloseKey(h2); }
    }
    MessageBox(g_hwnd, L"已注册到「打开方式」列表。\n右键文件 → 打开方式 → 选择 LiteReader 即可。",
        L"LiteReader", MB_OK | MB_ICONINFORMATION);
}

// ----------------------------------------------------------------------------
// 多标签操作
// ----------------------------------------------------------------------------
// 弹出“打开文件”对话框，返回选中的文件路径（取消则返回空串）。
std::wstring openFileDialog() {
    OPENFILENAME ofn = { sizeof(ofn) };
    wchar_t buf[MAX_PATH] = { 0 };
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter =
        L"文本/"
        L"代码\0*.txt;*.cs;*.sql;*.html;*.htm;*.xml;*.js;*.json;*.css;*.py;*.pyw\0所有文件\0*.*\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileName(&ofn)) return std::wstring(buf);
    return L"";
}

// 在新标签中打开文件：已打开则切换；否则新建标签并载入。
void openInNewTab(const std::wstring& path) {
    if (path.empty()) return;
    // 去重：已打开则切换
    for (size_t i = 0; i < g_docs.size(); i++) {
        if (g_docs[i].filePath == path) {
            switchTab((int)i);
            return;
        }
    }
    if (g_active >= 0) snapshotTo(g_active); // 先把当前标签状态存好
    g_docs.push_back(Doc());
    g_active = (int)g_docs.size() - 1;
    loadFile(path);
    g_docs[g_active].filePath = g_filePath;
    ensureActiveTabVisible(); // 新标签滚入可视区
    InvalidateRect(g_hwnd, NULL, TRUE);
}

// 切换到标签 j：保存当前、恢复目标、重算布局、重绘。
void switchTab(int j) {
    if (j == g_active || j < 0 || j >= (int)g_docs.size()) return;
    snapshotTo(g_active);
    g_active = j;
    restoreFrom(j);
    g_markWord.clear();
    g_markFlag.clear();
    g_markRanges.clear(); // 切换标签，清除上一个文档的分词高亮
    buildVisual();
    updateScroll();
    ensureActiveTabVisible(); // 切换后当前标签滚入可视区
    InvalidateRect(g_hwnd, NULL, TRUE);
    updateCaretPos();
    setWindowTitle();
}

// 关闭标签 i：仅剩一个时清空内容；否则删除并切换到相邻标签。
void closeTab(int i) {
    if (i < 0 || i >= (int)g_docs.size()) return;
    g_markWord.clear();
    g_markFlag.clear();
    g_markRanges.clear(); // 关闭标签，清除分词高亮
    int n = (int)g_docs.size();
    if (n <= 1) {
        // 仅剩一个：清空
        g_text.clear();
        g_lineStart.clear();
        g_lineLen.clear();
        g_lineDepth.clear();
        g_lineInBC.clear();
        g_lineInSrv.clear();
        g_lineInBlock.clear();
        g_lineBsQ.clear();
        g_tokens.clear();
        g_stateDone.clear();
        g_lineCount = 0;
        g_filePath.clear();
        g_lang = L"auto";
        g_langId = L_AUTO;
        g_caretOff = 0;
        g_anchorOff = 0;
        g_selStart = -1;
        g_selEnd = -1;
        g_matchA = -1;
        g_matchB = -1;
        g_matchAw = 1;
        g_matchBw = 1;
        g_topLine = 0;
        g_scrollX = 0;
        g_visual.clear();
        g_visualCount = 0;
        g_docs[0] = Doc();
        g_undoStack.clear();
        g_redoStack.clear(); // 内容清空：撤销/重做历史作废
        rebuildLines();
        buildVisual();
        updateScroll();
        InvalidateRect(g_hwnd, NULL, TRUE);
        updateCaretPos();
        setWindowTitle();
        return;
    }
    if (i == g_active) {
        int neighbor = (i > 0) ? i - 1 : 1; // 关闭当前标签后，激活其左侧（或右侧）邻居
        g_docs.erase(g_docs.begin() + i);
        g_active = neighbor;
        if (g_active >= (int)g_docs.size()) g_active = (int)g_docs.size() - 1;
        if (g_active < 0) g_active = 0;
        restoreFrom(g_active);
        buildVisual();
        updateScroll();
        ensureActiveTabVisible();
        InvalidateRect(g_hwnd, NULL, TRUE);
        updateCaretPos();
        setWindowTitle();
    }
    else {
        g_docs.erase(g_docs.begin() + i);
        if (i < g_active) g_active--; // 删除的是当前标签之前的，索引需前移
    }
    ensureActiveTabVisible();
    InvalidateRect(g_hwnd, NULL, TRUE);
}

// ----------------------------------------------------------------------------
// 菜单
// ----------------------------------------------------------------------------
// 创建主菜单：文件 / 编辑 / 视图（含语言子菜单）。菜单项 id 与 WM_COMMAND 中对应。
// 注意：为让菜单栏能随主题变色，系统菜单条被废弃；这里只创建三个弹出菜单，
//       菜单栏本身在客户区顶部自绘（drawMenuBar）。
void createMenus() {
    if (g_hMenuFile) {
        DestroyMenu(g_hMenuFile);
        g_hMenuFile = NULL;
    }
    if (g_hMenuEdit) {
        DestroyMenu(g_hMenuEdit);
        g_hMenuEdit = NULL;
    }
    if (g_hMenuView) {
        DestroyMenu(g_hMenuView);
        g_hMenuView = NULL;
    }
    g_hMenuFile = CreatePopupMenu();
    AppendMenu(g_hMenuFile, MF_STRING, 1001, L"打开...\tCtrl+O");
    AppendMenu(g_hMenuFile, MF_STRING, 1007, L"打开文件夹...\tCtrl+Shift+O");
    AppendMenu(g_hMenuFile, MF_STRING, 1004, L"新建标签\tCtrl+T");
    AppendMenu(g_hMenuFile, MF_STRING, 1005, L"关闭标签\tCtrl+W");
    AppendMenu(g_hMenuFile, MF_STRING, 1006, L"保存\tCtrl+S");
    AppendMenu(g_hMenuFile, MF_STRING, 1008, L"关闭文件夹");
    AppendMenu(g_hMenuFile, MF_STRING, 1002, L"设为默认打开程序");
    AppendMenu(g_hMenuFile, MF_SEPARATOR, 0, NULL);
    AppendMenu(g_hMenuFile, MF_STRING, 1003, L"退出");

    g_hMenuEdit = CreatePopupMenu();
    AppendMenu(g_hMenuEdit, MF_STRING, 1101, L"复制\tCtrl+C");
    AppendMenu(g_hMenuEdit, MF_STRING, 1102, L"全选\tCtrl+A");
    AppendMenu(g_hMenuEdit, MF_STRING, 1103, L"查找\tCtrl+F");

    g_hMenuView = CreatePopupMenu();
    HMENU hTheme = CreatePopupMenu();
    AppendMenu(hTheme, MF_STRING, 1250, L"One Dark Pro");
    AppendMenu(hTheme, MF_STRING, 1251, L"One Light");
    AppendMenu(hTheme, MF_STRING, 1252, L"VS Code");
    AppendMenu(hTheme, MF_STRING, 1253, L"IntelliJ IDEA");
    AppendMenu(hTheme, MF_STRING, 1254, L"极致黑");
    AppendMenu(g_hMenuView, MF_POPUP, (UINT_PTR)hTheme, L"主题");
    AppendMenu(g_hMenuView, MF_STRING, 1202, L"自动换行");
    AppendMenu(g_hMenuView, MF_STRING, 1260, L"代码补全");
    AppendMenu(g_hMenuView, MF_STRING, 1203, L"字体 +");
    AppendMenu(g_hMenuView, MF_STRING, 1204, L"字体 -");
    HMENU hLang = CreatePopupMenu();
    AppendMenu(hLang, MF_STRING, 1300, L"自动");
    AppendMenu(hLang, MF_STRING, 1301, L"纯文本");
    AppendMenu(hLang, MF_STRING, 1302, L"C#");
    AppendMenu(hLang, MF_STRING, 1303, L"SQL");
    AppendMenu(hLang, MF_STRING, 1304, L"HTML");
    AppendMenu(hLang, MF_STRING, 1305, L"JavaScript");
    AppendMenu(hLang, MF_STRING, 1306, L"JSON");
    AppendMenu(hLang, MF_STRING, 1307, L"Python");
    AppendMenu(hLang, MF_STRING, 1308, L"CSS");
    AppendMenu(hLang, MF_STRING, 1309, L"C");
    AppendMenu(hLang, MF_STRING, 1310, L"C++");
    AppendMenu(hLang, MF_STRING, 1311, L"Java");
    AppendMenu(hLang, MF_STRING, 1312, L"ASPX");
    AppendMenu(hLang, MF_STRING, 1313, L"XML");
    AppendMenu(g_hMenuView, MF_POPUP, (UINT_PTR)hLang, L"语言");
}
// 刷新三个弹出菜单的勾选状态：当前语言项、自动换行项、主题项。
void checkMenus() {
    UINT langMap[] = { 1300, 1301, 1302, 1303, 1304, 1305, 1306,
                      1307, 1308, 1309, 1310, 1311, 1312, 1313 };
    int idx = 0;
    if (g_lang == L"auto")
        idx = 0;
    else if (g_lang == L"txt")
        idx = 1;
    else if (g_lang == L"csharp")
        idx = 2;
    else if (g_lang == L"sql")
        idx = 3;
    else if (g_lang == L"html")
        idx = 4;
    else if (g_lang == L"js")
        idx = 5;
    else if (g_lang == L"json")
        idx = 6;
    else if (g_lang == L"python")
        idx = 7;
    else if (g_lang == L"css")
        idx = 8;
    else if (g_lang == L"c")
        idx = 9;
    else if (g_lang == L"cpp")
        idx = 10;
    else if (g_lang == L"java")
        idx = 11;
    else if (g_lang == L"aspx")
        idx = 12;
    else if (g_lang == L"xml")
        idx = 13;
    HMENU hTheme = GetSubMenu(g_hMenuView, 0); // 主题子菜单（视图内第 1 项，索引 0）
    HMENU hLang = GetSubMenu(g_hMenuView, 3);  // 语言子菜单（视图内第 4 项，索引 3）
    for (int i = 0; i < 14; i++)
        CheckMenuItem(hLang, langMap[i], (i == idx) ? MF_CHECKED : MF_UNCHECKED);
    for (int i = 0; i < NTHEMES; i++)
        CheckMenuItem(hTheme, 1250 + i, (i == g_themeIdx) ? MF_CHECKED : MF_UNCHECKED);
    CheckMenuItem(g_hMenuView, 1202, g_wrap ? MF_CHECKED : MF_UNCHECKED);         // 自动换行勾选
    CheckMenuItem(g_hMenuView, 1260, g_autocomplete ? MF_CHECKED : MF_UNCHECKED); // 代码补全勾选
}

// ----------------------------------------------------------------------------
// 顶部自绘菜单栏
// ----------------------------------------------------------------------------
static const wchar_t* g_menuLabels[3] = { L"文件", L"编辑", L"视图" };
static const int g_menuWidths[3] = { 48, 48, 48 }; // 暂定等宽，drawMenuBar 内按文字实际测量

int menuBarHit(int x, const RECT& rc) {
    int x0 = 8;
    for (int i = 0; i < 3; i++) {
        int w = g_menuRects[i].right - g_menuRects[i].left; // 实际宽度由 drawMenuBar 写入
        if (x >= g_menuRects[i].left && x < g_menuRects[i].right) return i;
    }
    return -1;
}

void drawMenuBar(HDC mem, const RECT& rc) {
    // 背景：使用主题标题栏/菜单栏色
    RECT mbr = { 0, 0, rc.right, MENU_H };
    HBRUSH b = CreateSolidBrush(TH.menuBarBg);
    FillRect(mem, &mbr, b);
    DeleteObject(b);
    // 底部分隔线
    HPEN sp = CreatePen(PS_SOLID, 1, TH.menuBarDivider);
    HPEN op = (HPEN)SelectObject(mem, sp);
    MoveToEx(mem, 0, MENU_H - 1, NULL);
    LineTo(mem, rc.right, MENU_H - 1);
    SelectObject(mem, op);
    DeleteObject(sp);

    SetBkMode(mem, TRANSPARENT);
    HFONT oldF = (HFONT)SelectObject(mem, g_sideFont ? g_sideFont : g_hFont);
    int x0 = 8;
    for (int i = 0; i < 3; i++) {
        SIZE sz;
        GetTextExtentPoint32(mem, g_menuLabels[i], (int)wcslen(g_menuLabels[i]), &sz);
        int w = sz.cx + 20;
        int h = MENU_H;
        g_menuRects[i] = { x0, 0, x0 + w, h };
        bool hover = (g_menuHover == i || g_menuActive == i);
        if (hover) {
            HBRUSH hb = CreateSolidBrush(TH.menuBarHover);
            FillRect(mem, &g_menuRects[i], hb);
            DeleteObject(hb);
        }
        SetTextColor(mem, hover ? TH.menuBarTextHover : TH.menuBarText);
        RECT tr = g_menuRects[i];
        DrawText(mem, g_menuLabels[i], (int)wcslen(g_menuLabels[i]), &tr,
            DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        x0 += w;
    }
    if (oldF) SelectObject(mem, oldF);
}

void showMenuPopup(int idx, HWND hwnd, const RECT& rc) {
    if (idx < 0 || idx > 2) return;
    HMENU popup = (idx == 0) ? g_hMenuFile : (idx == 1) ? g_hMenuEdit : g_hMenuView;
    checkMenus();
    POINT pt = { g_menuRects[idx].left, MENU_H };
    ClientToScreen(hwnd, &pt);
    g_menuActive = idx;
    UINT flags = TPM_LEFTALIGN | TPM_TOPALIGN | TPM_LEFTBUTTON;
    // 自绘菜单栏没有系统菜单条，需要手动让弹出菜单在失去焦点/点击外部时消失；TrackPopupMenu 已处理。
    TrackPopupMenu(popup, flags, pt.x, pt.y, 0, hwnd, NULL);
    g_menuActive = -1;
    g_menuHover = -1;
    RECT rr = { 0, 0, rc.right, MENU_H };
    InvalidateRect(hwnd, &rr, FALSE); // 关闭菜单后重绘菜单栏
}

// ----------------------------------------------------------------------------
// 右键菜单与标签批量关闭
// ----------------------------------------------------------------------------
// 复制当前选区到剪贴板（无选区则无操作）
void copySelection() {
    if (g_selStart < 0) return;
    std::wstring sub = g_text.substr(g_selStart, g_selEnd - g_selStart);
    HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, (sub.size() + 1) * sizeof(wchar_t));
    if (!hg) return;
    wchar_t* p = (wchar_t*)GlobalLock(hg);
    wcscpy_s(p, sub.size() + 1, sub.c_str());
    GlobalUnlock(hg);
    OpenClipboard(g_hwnd);
    EmptyClipboard();
    SetClipboardData(CF_UNICODETEXT, hg);
    CloseClipboard();
}
// 剪切：复制后删除选区（复用 insertText 空串，自动记录撤销）
void cutSelection() {
    if (g_selStart < 0) return;
    copySelection();
    insertText(L"");
}
// 粘贴：读取剪贴板文本并插入（替换选区，自动记录撤销）
void pasteFromClipboard() {
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) return;
    if (!OpenClipboard(g_hwnd)) return;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (h) {
        wchar_t* p = (wchar_t*)GlobalLock(h);
        if (p) {
            insertText(std::wstring(p));
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
}
// 全选
void selectAll() {
    g_selStart = 0;
    g_selEnd = (int)g_text.size();
    g_anchorOff = 0;
    g_caretOff = (int)g_text.size();
    InvalidateRect(g_hwnd, NULL, TRUE);
}
// 编辑区右键：在点击处放置光标；若已有选区且点击落在选区内则保留选区（方便复制）
void placeCaretForContext(int x, int y) {
    int ey = y - editorTop();
    int v = ey / g_lineH + g_topLine;
    if (v < 0 || v >= g_visualCount) return;
    const Visual& vis = g_visual[v];
    int target = (g_wrap ? linePrefixPx(vis.line, vis.col) : 0) + (x - leftBar() - g_gutterW) +
        (g_wrap ? 0 : g_scrollX);
    int col = pxToColAbs(vis.line, target);
    if (col < vis.col) col = vis.col;
    if (col > vis.col + vis.len) col = vis.col + vis.len;
    if (col < 0) col = 0;
    if (col > g_lineLen[vis.line]) col = g_lineLen[vis.line];
    int off = g_lineStart[vis.line] + col;
    if (g_selStart >= 0 && off >= g_selStart && off < g_selEnd) return; // 落在原选区内：保留选区
    g_anchorOff = off;
    g_caretOff = off;
    g_selStart = -1;
    g_selEnd = -1;
    findMatch();
    updateCaretPos();
    InvalidateRect(g_hwnd, NULL, TRUE);
}
// 由 x 坐标反推标签索引（与 drawTabBar 布局一致）
int tabIndexAt(int x) {
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    int x0 = TAB_X0 - g_tabScroll;
    for (int i = 0; i < (int)g_docs.size(); i++) {
        int w = tabWidthFor(i);
        if (x >= x0 && x < x0 + w) return i;
        x0 += w;
    }
    return -1;
}
// 批量关闭：仅保留 keep 一个标签
void closeOtherTabs(int keep) {
    int n = (int)g_docs.size();
    if (n <= 1 || keep < 0 || keep >= n) return;
    if (g_active != keep) {
        g_active = keep;
        restoreFrom(g_active);
    }
    Doc keepDoc = std::move(g_docs[keep]);
    g_docs.clear();
    g_docs.push_back(std::move(keepDoc));
    g_active = 0;
    g_topLine = 0;
    g_scrollX = 0;
    invalidateLineCache();
    ensureActiveTabVisible();
    buildVisual();
    updateScroll();
    InvalidateRect(g_hwnd, NULL, TRUE);
    updateCaretPos();
    setWindowTitle();
}
// 批量关闭：保留 idx 及其右侧
void closeLeftTabs(int idx) {
    int n = (int)g_docs.size();
    if (idx <= 0 || idx >= n) return;
    if (g_active != idx) {
        g_active = idx;
        restoreFrom(g_active);
    }
    std::vector<Doc> kept;
    for (int i = idx; i < n; i++) kept.push_back(std::move(g_docs[i]));
    g_docs.swap(kept);
    g_active = 0;
    g_topLine = 0;
    g_scrollX = 0;
    invalidateLineCache();
    ensureActiveTabVisible();
    buildVisual();
    updateScroll();
    InvalidateRect(g_hwnd, NULL, TRUE);
    updateCaretPos();
    setWindowTitle();
}
// 批量关闭：保留 idx 及其左侧
void closeRightTabs(int idx) {
    int n = (int)g_docs.size();
    if (idx < 0 || idx >= n - 1) return;
    if (g_active > idx) {
        g_active = idx;
        restoreFrom(g_active);
    }
    g_docs.erase(g_docs.begin() + idx + 1, g_docs.end());
    if (g_active > idx) g_active = idx;
    g_topLine = 0;
    g_scrollX = 0;
    invalidateLineCache();
    ensureActiveTabVisible();
    buildVisual();
    updateScroll();
    InvalidateRect(g_hwnd, NULL, TRUE);
    updateCaretPos();
    setWindowTitle();
}
// 弹出标签右键菜单（关闭左侧/右侧/全部关闭保留选中）
void showTabMenu(HWND hwnd, int sx, int sy, int idx) {
    HMENU m = CreatePopupMenu();
    AppendMenu(m, MF_STRING, 1501, L"关闭左侧");
    AppendMenu(m, MF_STRING, 1502, L"关闭右侧");
    AppendMenu(m, MF_STRING, 1503, L"全部关闭保留选中");
    if (idx == 0) EnableMenuItem(m, 1501, MF_GRAYED);
    if (idx == (int)g_docs.size() - 1) EnableMenuItem(m, 1502, MF_GRAYED);
    if (g_docs.size() <= 1) EnableMenuItem(m, 1503, MF_GRAYED);
    g_ctxTab = idx;
    TrackPopupMenu(m, TPM_RIGHTBUTTON, sx, sy, 0, hwnd, NULL);
    DestroyMenu(m);
}
// 弹出编辑区右键菜单（复制/剪切/粘贴/全选）
void showEditorMenu(HWND hwnd, int sx, int sy) {
    HMENU m = CreatePopupMenu();
    AppendMenu(m, MF_STRING, 1401, L"复制\tCtrl+C");
    AppendMenu(m, MF_STRING, 1402, L"剪切\tCtrl+X");
    AppendMenu(m, MF_STRING, 1403, L"粘贴\tCtrl+V");
    AppendMenu(m, MF_SEPARATOR, 0, NULL);
    AppendMenu(m, MF_STRING, 1404, L"全选\tCtrl+A");
    AppendMenu(m, MF_SEPARATOR, 0, NULL);
    AppendMenu(m, MF_STRING, 1405, L"在命令提示符中打开");
    AppendMenu(m, MF_SEPARATOR, 0, NULL);
    AppendMenu(m, MF_STRING, 1406, L"跳转到定义\tF12");
    if (!(g_selStart >= 0)) {
        EnableMenuItem(m, 1401, MF_GRAYED);
        EnableMenuItem(m, 1402, MF_GRAYED);
    }
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) EnableMenuItem(m, 1403, MF_GRAYED);
    if (selectedOrWordAtCaret().empty()) EnableMenuItem(m, 1406, MF_GRAYED);
    TrackPopupMenu(m, TPM_RIGHTBUTTON, sx, sy, 0, hwnd, NULL);
    DestroyMenu(m);
}

// 打开外部终端：优先 Windows Terminal（wt.exe），其次 PowerShell，最后 cmd。
// 工作目录取“当前焦点标签文件所在目录”。
static bool launchTerminal(const wchar_t* exe, const std::wstring& args, const std::wstring& dir) {
    HINSTANCE r = ShellExecuteW(NULL, L"open", exe, args.empty() ? NULL : args.c_str(), dir.c_str(),
        SW_SHOWNORMAL);
    return (INT_PTR)r > 32; // ShellExecute 返回 >32 表示成功
}
// 当前焦点标签文件所在目录（无文件时回退“我的文档”）
std::wstring currentDir() {
    if (!g_filePath.empty()) {
        size_t pos = g_filePath.find_last_of(L"\\/");
        return (pos != std::wstring::npos) ? g_filePath.substr(0, pos) : g_filePath;
    }
    wchar_t buf[MAX_PATH] = { 0 };
    return (SHGetFolderPathW(NULL, CSIDL_PERSONAL, NULL, 0, buf) == S_OK) ? std::wstring(buf)
        : std::wstring(L"C:\\");
}
// 打开外部终端：优先 Windows Terminal（wt.exe），其次 PowerShell，最后 cmd。
// 任一成功即返回 true；全部失败返回 false。
static bool openTerminalHere() {
    std::wstring dir = currentDir();
    if (launchTerminal(L"wt.exe", L"-d \"" + dir + L"\"", dir))
        return true; // Windows Terminal：用 -d 指定起始目录
    if (launchTerminal(L"powershell.exe", L"", dir)) return true; // PowerShell：起始目录即工作目录
    if (launchTerminal(L"cmd.exe", L"", dir)) return true;        // 兜底 cmd
    return false;
}
// 查找“已经打开”的终端窗口：枚举顶层窗口，匹配控制台类（cmd/PowerShell 的 conhost）或 Windows
// Terminal
struct TermEnumCtx {
    HWND found;
};
static BOOL CALLBACK TermEnumProc(HWND hwnd, LPARAM lParam) {
    TermEnumCtx* c = (TermEnumCtx*)lParam;
    wchar_t cls[64] = { 0 };
    if (GetClassNameW(hwnd, cls, 64) > 0) {
        if (wcscmp(cls, L"ConsoleWindowClass") == 0 ||
            wcscmp(cls, L"CASCADIA_HOSTING_WINDOW_CLASS") == 0) {
            c->found = hwnd;
            return FALSE; // 命中即停止
        }
    }
    return TRUE;
}
static HWND findOpenTerminal() {
    TermEnumCtx c = { NULL };
    EnumWindows(TermEnumProc, (LPARAM)&c);
    return c.found;
}
// 把文本写入系统剪贴板
static void setClipboardText(const std::wstring& s) {
    if (s.empty()) return;
    if (!OpenClipboard(NULL)) return;
    EmptyClipboard();
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (s.size() + 1) * sizeof(wchar_t));
    if (h) {
        wchar_t* p = (wchar_t*)GlobalLock(h);
        wcscpy_s(p, s.size() + 1, s.c_str());
        GlobalUnlock(h);
        SetClipboardData(CF_UNICODETEXT, h);
    }
    CloseClipboard();
}
// 右键菜单「在命令提示符中打开」：打开（或复用已开）终端，把选中文字粘贴到命令行但不提交（不回车）
void openTerminalAndPasteSel() {
    std::wstring sel;
    if (g_selStart >= 0) sel = g_text.substr(g_selStart, g_selEnd - g_selStart);
    HWND term = findOpenTerminal();
    if (!term) {                          // 没有已开的终端 → 在本文件目录新开一个
        AllowSetForegroundWindow(ASFW_ANY); // 允许新启动的终端进程抢占前台（绕过前台锁）
        bool ok = openTerminalHere();
        if (ok) { // 轮询等待终端窗口真正出现（最多 ~2 秒），避免固定 500ms 可能错过启动较慢的终端
            for (int i = 0; i < 20 && !term; i++) {
                Sleep(100);
                term = findOpenTerminal();
            }
        }
    }
    if (!term) return; // 仍找不到（如终端启动失败）→ 静默退出
    if (IsIconic(term)) ShowWindow(term, SW_RESTORE);
    ShowWindow(term, SW_SHOW);
    SetForegroundWindow(term); // 确保终端被拉到前台并获焦
    if (sel.empty()) return;   // 没有选中内容则仅打开，不粘贴
    setClipboardText(sel);     // 放到剪贴板，终端里 Ctrl+V 即粘贴（且不回车）
    Sleep(120);                // 等窗口拿到焦点并就绪
    INPUT inp[4] = { 0 };
    inp[0].type = INPUT_KEYBOARD;
    inp[0].ki.wVk = VK_CONTROL;
    inp[1].type = INPUT_KEYBOARD;
    inp[1].ki.wVk = 'V';
    inp[2].type = INPUT_KEYBOARD;
    inp[2].ki.wVk = 'V';
    inp[2].ki.dwFlags = KEYEVENTF_KEYUP;
    inp[3].type = INPUT_KEYBOARD;
    inp[3].ki.wVk = VK_CONTROL;
    inp[3].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(4, inp, sizeof(INPUT));
}

// ----------------------------------------------------------------------------
// 跳转到定义：在当前“文档内”按启发式查找标识符的“定义”位置并跳转。
// 适用范围：单文件内的函数/变量定义跳转（不跨文件、不做语义解析，纯文本扫描）。
// 判定规则（启发式打分，越高越像“定义”）：
//   1) 标识符随后紧跟 '(' 视为“函数式”            +3 分；
//   2) 前方修饰/类型关键字（public/void/int/def/class/function/const/let/var/
//      create/procedure/table/trigger/view 等）视为“声明” +5 分；
//   3) 前方是首字母大写的词（疑似类型名）          +2 分。
// 取全文中得分最高、且尽量非当前行的位置跳转，并选中该词便于确认。
// ----------------------------------------------------------------------------
static bool iequals(const std::wstring& a, const std::wstring& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) {
        if (towlower(a[i]) != towlower(b[i])) return false;
    }
    return true;
}
// 取“目标标识符”：优先用选区的首词，否则用光标下的整词
std::wstring selectedOrWordAtCaret() {
    if (g_selStart >= 0 && g_selEnd > g_selStart) {
        std::wstring sel = g_text.substr(g_selStart, g_selEnd - g_selStart);
        int a = 0, b = (int)sel.size();
        while (a < b && !isWordChar(sel[a])) a++;
        while (b > a && !isWordChar(sel[b - 1])) b--;
        if (a < b) return sel.substr(a, b - a);
        return L"";
    }
    int ws, we;
    wordAtOffset(g_caretOff, ws, we);
    if (we > ws) return g_text.substr(ws, we - ws);
    return L"";
}
// 取 off 之前的整词（不含 off 本身）
static std::wstring prevWordAt(int off) {
    int p = off - 1;
    while (p >= 0 && isWordChar(g_text[p])) p--;
    return g_text.substr(p + 1, off - (p + 1));
}
// 取 off 之后第一个非空白字符（用于判断 name 后是否跟 '('）
static wchar_t nextNonSpace(int off) {
    int e = off;
    while (e < (int)g_text.size() && (g_text[e] == L' ' || g_text[e] == L'\t')) e++;
    return (e < (int)g_text.size()) ? g_text[e] : 0;
}
// 对某一行某次整词命中打分（越高越像“定义”）
static int scoreDefinition(int line, int pos, int len) {
    int score = 1; // 基础分：任意整词命中
    std::wstring prev = prevWordAt(pos);
    wchar_t nxt = nextNonSpace(pos + len);
    if (nxt == L'(') score += 3; // 后面跟 ( => 函数式定义
    static const wchar_t* defKw[] = {
        L"def",      L"class",   L"function", L"func",      L"public",   L"private",   L"protected",
        L"static",   L"void",    L"int",      L"long",      L"short",    L"char",      L"float",
        L"double",   L"bool",    L"boolean",  L"string",    L"unsigned", L"signed",    L"const",
        L"let",      L"var",     L"create",   L"procedure", L"table",    L"trigger",   L"view",
        L"index",    L"struct",  L"enum",     L"interface", L"final",    L"abstract",  L"virtual",
        L"override", L"new",     L"friend",   L"inline",    L"operator", L"namespace", L"module",
        L"sub",      L"proc",    L"fn",       L"type",      L"local",    L"global",    L"dim",
        L"set",      L"property" };
    for (const wchar_t* k : defKw) {
        if (iequals(prev, k)) {
            score += 5;
            break;
        }
    }
    if (prev.size() >= 2 && iswupper((wchar_t)prev[0]) != 0)
        score += 2; // 前方首字母大写词（疑似类型名）
    return score;
}
// 跳转到 name 的“定义”：扫描全文挑选得分最高的位置并跳转
void gotoDefinition(const std::wstring& name) {
    if (name.empty()) return;
    int n = g_lineCount, lns = (int)name.size();
    int bestOff = -1, bestScore = 0;
    int caretLine = lineOfOffset(g_caretOff);
    for (int l = 0; l < n; l++) {
        int s = g_lineStart[l], e = s + g_lineLen[l];
        if (e - s < lns) continue;
        int p = s;
        while (p <= e - lns) {
            if (g_text.compare(p, lns, name) == 0) {
                bool okPrev = (p == 0) || !isWordChar(g_text[p - 1]);
                bool okNext = (p + lns >= e) || !isWordChar(g_text[p + lns]);
                if (okPrev && okNext) {
                    int sc = scoreDefinition(l, p, lns);
                    if (l == caretLine) sc -= 1; // 当前行略减分，偏向跳到别处的定义
                    if (sc > bestScore) {
                        bestScore = sc;
                        bestOff = p;
                    }
                }
                p += lns;
            }
            else
                p++;
        }
    }
    if (bestOff >= 0 && bestScore > 0) {
        g_selStart = bestOff;
        g_selEnd = bestOff + lns; // 选中目标词，便于确认
        setCaret(bestOff);        // setCaret 自动滚动并移动系统光标
    }
    else {
        MessageBeep(0xFFFFFFFF); // 无定义可跳：提示音
    }
}

// ----------------------------------------------------------------------------
// 代码补全：关键字补全 / 括号配对补全 / 函数补全
//   - 全部受 g_autocomplete 开关控制（.ini 的 autocomplete 项，默认关闭）。
//   - 关键字/类型补全：依据当前语言关键字表做前缀匹配。
//   - 函数补全：扫描当前文档内“标识符后紧跟 (”的名称，去重收集。
//   - 括号配对：输入 ( { [ " ' 时自动补出配对字符并把光标置于中间；
//     再次输入右配对字符、且其后恰为该字符时，直接跳过（不重复插入）。
//   - 候选列表为 WS_POPUP 弹出窗口；↑/↓ 选择、Tab/Enter 确认、Esc 取消、点击项确认。
// ----------------------------------------------------------------------------
static const std::set<std::wstring>* activeKeywordSet() {
    switch (g_langId) {
    case L_CS:
        return &KW_CS;
    case L_SQL:
        return &KW_SQL;
    case L_JS:
        return &KW_JS;
    case L_PY:
        return &KW_PY;
    case L_CSS:
        return &KW_CSS;
    case L_C:
        return &KW_C;
    case L_CPP:
        return &KW_CPP;
    case L_JAVA:
        return &KW_JAVA;
    default:
        return NULL;
    }
}
static const std::set<std::wstring>* activeTypeSet() {
    switch (g_langId) {
    case L_CS:
        return &TY_CS;
    case L_C:
        return &TY_C;
    case L_CPP:
        return &TY_CPP;
    case L_JAVA:
        return &TY_JAVA;
    default:
        return NULL;
    }
}
static bool startsWithCI(const std::wstring& s, const std::wstring& p) {
    if (p.empty() || p.size() > s.size()) return false;
    for (size_t i = 0; i < p.size(); i++)
        if (towlower((wchar_t)s[i]) != towlower((wchar_t)p[i])) return false;
    return true;
}
static bool isPairOpen(wchar_t c) {
    return c == L'(' || c == L'{' || c == L'[' || c == L'<' || c == L'"' || c == L'\'';
}
static bool isPairClose(wchar_t c) {
    return c == L')' || c == L'}' || c == L']' || c == L'>' || c == L'"' || c == L'\'';
}
static wchar_t pairClose(wchar_t c) {
    switch (c) {
    case L'(':
        return L')';
    case L'{':
        return L'}';
    case L'[':
        return L']';
    case L'<':
        return L'>';
    case L'"':
        return L'"';
    case L'\'':
        return L'\'';
    }
    return c;
}
// 重建文档内函数名列表（供函数补全）。最多扫描 2MB 以免超大文件卡顿；关键字不作为函数建议。
void rebuildFuncNames() {
    g_funcNames.clear();
    g_funcDirty = false;
    int len = (int)g_text.size();
    if (len == 0) return;
    int cap = len;
    if (cap > 2 * 1024 * 1024) cap = 2 * 1024 * 1024;
    const std::set<std::wstring>* kw = activeKeywordSet();
    std::set<std::wstring> seen;
    int i = 0;
    while (i < cap) {
        wchar_t c = g_text[i];
        if (isWordChar(c)) {
            int s = i;
            while (i < cap && isWordChar(g_text[i])) i++;
            int e = i;
            int j = e;
            while (j < cap &&
                (g_text[j] == L' ' || g_text[j] == L'\t' || g_text[j] == L'\n' || g_text[j] == L'\r'))
                j++;
            if (j < cap && g_text[j] == L'(') {
                std::wstring name = g_text.substr(s, e - s);
                if (!(kw && kw->count(name)) && seen.insert(name).second) g_funcNames.push_back(name);
                if ((int)g_funcNames.size() >= 2000) break;
            }
        }
        else
            i++;
    }
}
// 隐藏候选列表并清空候选
void hideCompletion() {
    if (g_compVisible) {
        g_compVisible = false;
        if (g_hComp) ShowWindow(g_hComp, SW_HIDE);
    }
    g_compItems.clear();
    g_compKind.clear();
    g_compSel = 0;
}
// 创建/定位/绘制候选列表弹出窗口
void showCompletion() {
    if (g_compItems.empty()) {
        hideCompletion();
        return;
    }
    int n = (int)g_compItems.size();
    int vis = (n < COMP_MAX_VISIBLE) ? n : COMP_MAX_VISIBLE;
    // 测量最宽候选以确定弹窗宽度
    int maxw = 0;
    HDC hdc = GetDC(g_hwnd);
    HFONT of = (HFONT)SelectObject(hdc, g_hFont ? g_hFont : (HFONT)GetStockObject(DEFAULT_GUI_FONT));
    for (int i = 0; i < n; i++) {
        SIZE sz;
        GetTextExtentPoint32(hdc, g_compItems[i].c_str(), (int)g_compItems[i].size(), &sz);
        if (sz.cx > maxw) maxw = sz.cx;
    }
    SelectObject(hdc, of);
    ReleaseDC(g_hwnd, hdc);
    int pad = 6, kindW = 26;
    int w = maxw + pad * 2 + kindW + 8;
    if (w < 160) w = 160;
    if (w > 360) w = 360;
    int h = vis * COMP_ITEM_H + 2;
    // 弹窗定位在光标下方（屏幕坐标）
    POINT cp;
    GetCaretPos(&cp);
    ClientToScreen(g_hwnd, &cp);
    int x = cp.x, y = cp.y + g_lineH + 1;
    int sh = GetSystemMetrics(SM_CYSCREEN);
    if (y + h > sh) y = cp.y - h - 1;
    if (y < 0) y = 0;
    if (!g_hComp) {
        if (!g_compClassRegistered) {
            registerCompClass();
            g_compClassRegistered = true;
        }
        g_hComp = CreateWindowEx(WS_EX_TOPMOST | WS_EX_NOACTIVATE, COMP_CLASS, NULL,
            WS_POPUP | WS_BORDER, x, y, w, h, g_hwnd, NULL, g_hInst, NULL);
        if (!g_hComp) {
            g_compVisible = false;
            return;
        }
    }
    else {
        SetWindowPos(g_hComp, NULL, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    g_compVisible = true;
    ShowWindow(g_hComp, SW_SHOWNOACTIVATE);
    RedrawWindow(g_hComp, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
}
// 确认当前高亮候选，用其替换正在输入的词（函数补全会自动补出 ()）
void acceptCompletion() {
    if (!g_compVisible) return;
    if (g_compSel < 0 || g_compSel >= (int)g_compItems.size()) {
        hideCompletion();
        return;
    }
    std::wstring full = g_compItems[g_compSel];
    int isFn = (g_compKind[g_compSel] == 1);
    int ws = g_compWordStart, we = g_compWordEnd;
    wchar_t nxt = (we < (int)g_text.size()) ? g_text[we] : 0;
    if (isFn && nxt != L'(' && nxt != L')') {
        // 常规情况：函数名后无现成括号，补出 ()
        std::wstring ins = full + L"()";
        applyEdit(ws, we, ins, true);
        g_caretOff = ws + (int)full.size() + 1; // 光标置于 () 之间
        updateCaretPos();
    }
    else if (isFn) {
        // 函数名后已紧跟 ( 或 )：仅替换名称，光标进入 () 内（不动输入焦点到词中）
        applyEdit(ws, we, full, true);
        g_caretOff = we;
        if (g_caretOff + 1 < (int)g_text.size() && g_text[g_caretOff] == L'(' &&
            g_text[g_caretOff + 1] == L')')
            g_caretOff++;
        else if (g_caretOff < (int)g_text.size() && g_text[g_caretOff] == L'(')
            g_caretOff++;
    }
    else {
        // 关键字/类型：补全整词后追加一个空格，光标落在空格之后，
        // 避免在补全词内部停留（修复：补全后焦点停在输入前的位置）
        applyEdit(ws, we, full + L" ", true);
    }
    hideCompletion();
}
// 根据光标处正在输入的词，收集关键字/函数候选并刷新弹窗
void updateCompletion() {
    if (!g_autocomplete) {
        hideCompletion();
        return;
    }
    int ws, we;
    wordAtOffset(g_caretOff, ws, we);
    std::wstring prefix = g_text.substr(ws, g_caretOff - ws);
    if (prefix.empty()) {
        hideCompletion();
        return;
    }
    std::vector<std::wstring> items;
    std::vector<int> kinds;
    const std::set<std::wstring>* kw = activeKeywordSet();
    const std::set<std::wstring>* ty = activeTypeSet();
    auto addSet = [&](const std::set<std::wstring>* s, int kind) {
        if (!s) return;
        for (const std::wstring& nm : *s) {
            if (startsWithCI(nm, prefix)) {
                items.push_back(nm);
                kinds.push_back(kind);
            }
        }
        };
    addSet(kw, 0);
    addSet(ty, 0);
    if (g_funcDirty) rebuildFuncNames();
    for (const std::wstring& nm : g_funcNames) {
        if (startsWithCI(nm, prefix)) {
            items.push_back(nm);
            kinds.push_back(1);
        }
    }
    if (items.empty()) {
        hideCompletion();
        return;
    }
    // 去重（保留首次出现），并限制总数
    std::vector<std::wstring> nitems;
    std::vector<int> nkinds;
    std::set<std::wstring> dup;
    for (size_t i = 0; i < items.size(); i++) {
        if (dup.insert(items[i]).second) {
            nitems.push_back(items[i]);
            nkinds.push_back(kinds[i]);
        }
    }
    const int MAXC = 600;
    if ((int)nitems.size() > MAXC) {
        nitems.resize(MAXC);
        nkinds.resize(MAXC);
    }
    g_compItems.swap(nitems);
    g_compKind.swap(nkinds);
    g_compWordStart = ws;
    g_compWordEnd = we;
    g_compSel = 0;
    showCompletion();
}
// 候选列表弹出窗口过程（自绘，仅显示/选择用）
LRESULT CALLBACK CompWndProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hw, &ps);
        RECT rc;
        GetClientRect(hw, &rc);
        HBRUSH bg = CreateSolidBrush(TH.bg);
        FillRect(hdc, &rc, bg);
        DeleteObject(bg);
        HPEN pen = CreatePen(PS_SOLID, 1, TH.menuBarDivider);
        HPEN op = (HPEN)SelectObject(hdc, pen);
        MoveToEx(hdc, 0, 0, NULL);
        LineTo(hdc, rc.right, 0);
        MoveToEx(hdc, 0, rc.bottom - 1, NULL);
        LineTo(hdc, rc.right, rc.bottom - 1);
        MoveToEx(hdc, 0, 0, NULL);
        LineTo(hdc, 0, rc.bottom);
        MoveToEx(hdc, rc.right - 1, 0, NULL);
        LineTo(hdc, rc.right - 1, rc.bottom);
        SelectObject(hdc, op);
        DeleteObject(pen);
        HFONT f = g_hFont ? g_hFont : (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        HFONT of = (HFONT)SelectObject(hdc, f);
        SetBkMode(hdc, TRANSPARENT);
        std::wstring prefix = g_text.substr(g_compWordStart, g_caretOff - g_compWordStart);
        int pad = 6, ty = (COMP_ITEM_H - g_lineH) / 2;
        if (ty < 0) ty = 0;
        for (int i = 0; i < (int)g_compItems.size(); i++) {
            int y = i * COMP_ITEM_H;
            RECT ir = { 1, y, rc.right - 1, y + COMP_ITEM_H };
            if (i == g_compSel) {
                HBRUSH hb = CreateSolidBrush(TH.selBg);
                FillRect(hdc, &ir, hb);
                DeleteObject(hb);
            }
            const std::wstring& s = g_compItems[i];
            int pre = (int)prefix.size();
            if (pre > (int)s.size()) pre = (int)s.size(); // 前缀不超过候选长度，避免越界
            bool isFn = (g_compKind[i] == 1);
            SetTextColor(hdc, isFn ? TH.c[T_FUNC] : TH.c[T_KEYWORD]); // 已输入前缀高亮（函数/关键字色）
            SIZE sz1;
            GetTextExtentPoint32(hdc, s.c_str(), pre, &sz1);
            TextOut(hdc, pad, y + ty, s.c_str(), pre);
            SetTextColor(hdc, TH.c[T_TEXT]); // 其余部分普通色
            int rest = (int)s.size() - pre;
            if (rest < 0) rest = 0;
            TextOut(hdc, pad + sz1.cx, y + ty, s.c_str() + pre, rest);
            const wchar_t* tag = isFn ? L"fn" : L"kw"; // 右侧类别标签
            SIZE sz2;
            GetTextExtentPoint32(hdc, tag, (int)wcslen(tag), &sz2);
            SetTextColor(hdc, TH.menuBarDivider);
            TextOut(hdc, rc.right - pad - sz2.cx, y + ty, tag, (int)wcslen(tag));
        }
        SelectObject(hdc, of);
        EndPaint(hw, &ps);
        return 0;
    }
    if (msg == WM_LBUTTONDOWN) {
        int y = (int)(short)HIWORD(lp);
        int idx = y / COMP_ITEM_H;
        if (idx >= 0 && idx < (int)g_compItems.size()) {
            g_compSel = idx;
            acceptCompletion();
        }
        return 0;
    }
    return DefWindowProc(hw, msg, wp, lp);
}
void registerCompClass() {
    WNDCLASSEX wc = { sizeof(wc) };
    wc.style = CS_SAVEBITS;
    wc.lpfnWndProc = CompWndProc;
    wc.hInstance = g_hInst;
    wc.hbrBackground = CreateSolidBrush(TH.bg);
    wc.lpszClassName = COMP_CLASS;
    RegisterClassEx(&wc);
}

// ----------------------------------------------------------------------------
// 主窗口过程
// 处理所有窗口消息：创建、绘制、滚动、鼠标、键盘、菜单、命令、销毁等。
// ----------------------------------------------------------------------------
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        DragAcceptFiles(hwnd, TRUE); // 允许拖拽文件到窗口
        g_docs.push_back(Doc());
        g_active = 0;
        ensureFont();
        buildVisual();
        updateScroll();
        return 0;
    }
    case WM_DROPFILES: {
        HDROP h = (HDROP)wp;
        wchar_t buf[MAX_PATH];
        if (DragQueryFile(h, 0, buf, MAX_PATH)) { openInNewTab(buf); } // 取拖入的第一个文件
        DragFinish(h);
        return 0;
    }
    case WM_SIZE: {
        hideCompletion(); // 窗口尺寸变化，收起补全列表
        ensureFont();
        if (g_wrap) {
            buildVisual();
            invalidateLineCache();
        } // 换行模式视觉行随宽度变化需重建+失效缓存；非换行视觉行与宽度无关
        updateScroll();
        clampTreeScroll(); // 窗口尺寸变化后，侧栏树滚动范围需重新夹紧
        {
            RECT r;
            GetClientRect(hwnd, &r); // 侧栏宽度不能超过窗口，且夹在最小/最大之间
            int maxW = r.right - 60;
            if (g_sidebarW > maxW) g_sidebarW = maxW;
            if (g_sidebarW < SIDEBAR_W_MIN) g_sidebarW = SIDEBAR_W_MIN;
        }
        clampTabScroll(); // 窗口变宽后把标签滚回可视区
        InvalidateRect(hwnd, NULL, TRUE);
        if (g_hFind) toggleFind(); // 窗口尺寸变化会导致子控件错位，先收起查找条
        return 0;
    }
    case WM_VSCROLL: {
        hideCompletion(); // 垂直滚动，收起补全列表
        int pos = GetScrollPos(hwnd, SB_VERT);
        RECT r;
        GetClientRect(hwnd, &r);
        int page = (r.bottom - editorTop()) / g_lineH;
        int m = LOWORD(wp);
        if (m == SB_LINEUP)
            pos--;
        else if (m == SB_LINEDOWN)
            pos++;
        else if (m == SB_PAGEUP)
            pos -= page;
        else if (m == SB_PAGEDOWN)
            pos += page;
        else if (m == SB_THUMBTRACK)
            pos = HIWORD(wp);
        else if (m == SB_THUMBPOSITION)
            pos = HIWORD(wp);
        if (pos < 0) pos = 0;
        if (pos > g_visualCount - 1) pos = g_visualCount - 1;
        if (pos != g_topLine) scrollByLines(pos - g_topLine); // 仅平移像素 + 重绘露出条带
        SetScrollPos(hwnd, SB_VERT, pos, TRUE);
        return 0;
    }
    case WM_HSCROLL: {
        hideCompletion(); // 水平滚动，收起补全列表
        int pos = GetScrollPos(hwnd, SB_HORZ);
        int m = LOWORD(wp);
        if (m == SB_LINELEFT)
            pos -= g_charW * 4;
        else if (m == SB_LINERIGHT)
            pos += g_charW * 4;
        else if (m == SB_PAGELEFT)
            pos -= 80;
        else if (m == SB_PAGERIGHT)
            pos += 80;
        else if (m == SB_THUMBTRACK)
            pos = HIWORD(wp);
        else if (m == SB_THUMBPOSITION)
            pos = HIWORD(wp);
        if (pos < 0) pos = 0;
        int maxX = g_maxLineW; // 缓存最大行宽，避免每行 linePx 全量扫描
        RECT r;
        GetClientRect(hwnd, &r);
        int hmax = maxX - (r.right - r.left - leftBar() - g_gutterW);
        if (hmax < 0) hmax = 0;
        if (pos > hmax) pos = hmax;
        g_scrollX = pos;
        SetScrollPos(hwnd, SB_HORZ, pos, TRUE);
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;
    }
    case WM_CONTEXTMENU: {
        int sx = (int)(short)LOWORD(lp), sy = (int)(short)HIWORD(lp); // 屏幕坐标
        POINT pt = { sx, sy };
        ScreenToClient(hwnd, &pt);
        int x = pt.x, y = pt.y;
        // 菜单栏/标签栏区域：按右击位置定位标签，弹出标签菜单
        if (y < MENU_H + TAB_H) {
            int idx = tabIndexAt(x);
            if (idx >= 0) showTabMenu(hwnd, sx, sy, idx);
            return 0;
        }
        // 编辑区（含行号区/查找条下方，且不在侧栏内）：弹出编辑菜单，并在点击处放置光标
        if (y >= editorTop() && x >= leftBar()) {
            placeCaretForContext(x, y);
            showEditorMenu(hwnd, sx, sy);
            return 0;
        }
        return 0; // 侧栏等其它区域不弹出菜单
    }
    case WM_MOUSEWHEEL: {
        hideCompletion(); // 滚轮滚动，收起补全列表
        int sx = (int)(short)LOWORD(lp), sy = (int)(short)HIWORD(lp);
        POINT pt = { sx, sy };
        ScreenToClient(hwnd, &pt);
        // Ctrl+滚轮：调整字号（每格 ±1pt，范围 9~28）
        if (GetKeyState(VK_CONTROL) & 0x8000) {
            int d = GET_WHEEL_DELTA_WPARAM(wp);
            g_fontSize += (d > 0) ? 1 : -1;
            if (g_fontSize < 9) g_fontSize = 9;
            if (g_fontSize > 28) g_fontSize = 28;
            ensureFont();
            buildVisual();
            updateScroll();
            InvalidateRect(hwnd, NULL, TRUE);
            updateCaretPos();
            return 0;
        }
        // 光标在菜单栏/标签栏上：水平滚动标签条
        if (pt.y < MENU_H + TAB_H) {
            int d = GET_WHEEL_DELTA_WPARAM(wp);
            g_tabScroll += (d > 0 ? 1 : -1) * 60;
            clampTabScroll();
            InvalidateRect(hwnd, NULL, TRUE);
            return 0;
        }
        // 光标在侧栏内：滚动文件夹树
        if (g_folderOpen && pt.x < leftBar() && pt.y >= editorTop()) {
            int delta = GET_WHEEL_DELTA_WPARAM(wp);
            int lines = (delta / WHEEL_DELTA) * (-3);
            g_treeScroll += lines * SIDEBAR_ROW_H;
            clampTreeScroll();
            InvalidateRect(hwnd, NULL, TRUE);
            return 0;
        }
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        int lines = (delta / WHEEL_DELTA) * (-3); // 每格滚轮滚动 3 行（方向取反）
        int pos = g_topLine + lines;
        if (pos < 0) pos = 0;
        if (pos > g_visualCount - 1) pos = g_visualCount - 1;
        if (pos != g_topLine) scrollByLines(pos - g_topLine);
        SetScrollPos(hwnd, SB_VERT, pos, TRUE);
        return 0;
    }
    case WM_COPYDATA: {
        // 来自另一个已存在实例：把命令行传入的文件路径在新实例中打开，并激活窗口
        PCOPYDATASTRUCT pc = (PCOPYDATASTRUCT)lp;
        if (pc && pc->dwData == 1 && pc->lpData) {
            std::wstring path = (wchar_t*)pc->lpData;
            if (!path.empty()) openInNewTab(path);
            bringToFront(hwnd);
        }
        return TRUE;
    }
    case WM_PAINT:
        paint();
        return 0;
    case WM_ERASEBKGND:
        return 1; // 由 paint 自己画背景，禁止系统擦除（避免闪烁）
    case WM_SETFOCUS:
        updateCaretPos();
        return 0;
    case WM_KILLFOCUS:
        hideCompletion();
        DestroyCaret();
        return 0;
    case WM_LBUTTONDOWN: {
        hideCompletion(); // 点击编辑区，收起补全列表
        int x = (int)LOWORD(lp), y = (int)HIWORD(lp);
        // 顶部自绘菜单栏
        if (y < MENU_H) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            int idx = menuBarHit(x, rc);
            if (idx >= 0) {
                showMenuPopup(idx, hwnd, rc);
                return 0;
            }
            return 0;
        }
        // 标签栏区域：判断点中“新建”按钮、关闭按钮还是切换标签（标签宽度按名称动态计算）
        if (y < MENU_H + TAB_H) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            int plusX = rc.right - PLUS_W;
            POINT pt = { x, y };
            // “新建标签”按钮（+）钉在最右侧，始终可见
            RECT pr = { plusX + 2, MENU_H + 2, plusX + PLUS_W - 2, MENU_H + TAB_H - 2 };
            if (PtInRect(&pr, pt)) {
                std::wstring p = openFileDialog();
                if (!p.empty()) openInNewTab(p);
                return 0;
            }
            int stripRight = rc.right - PLUS_W;
            int cx = TAB_X0 - g_tabScroll;
            int n = (int)g_docs.size();
            for (int i = 0; i < n; i++) {
                int w = tabWidthFor(i);
                if (cx + w <= 0) {
                    cx += w;
                    continue;
                }                            // 完全在可视区左侧外
                if (cx >= stripRight) break; // 已超出条带右界
                if (x >= cx && x < cx + w) {
                    RECT cr = { cx + w - TAB_CLOSE_W, MENU_H + 4, cx + w - 4, MENU_H + TAB_H - 4 };
                    if (PtInRect(&cr, pt)) {
                        closeTab(i);
                        return 0;
                    } // 点中关闭 ×
                    switchTab(i);
                    return 0; // 否则切换标签
                }
                cx += w;
            }
            return 0;
        }
        // 侧栏右缘拖动调宽（命中热区 [lb-1, lb+3]，避开树滚动条与行点击）
        if (g_folderOpen && y >= MENU_H + TAB_H) {
            int lb = leftBar();
            if (x >= lb - 1 && x <= lb + 3) {
                g_sidebarResizing = true;
                g_sidebarResizeStartX = x;
                g_sidebarResizeStartW = g_sidebarW;
                SetCapture(hwnd);
                return 0;
            }
        }
        // 左侧文件夹树区域（在标签栏下方、查找条/编辑器左侧）
        if (g_folderOpen && x < leftBar() && y >= editorTop()) {
            sidebarDown(x, y);
            return 0;
        }
        // 查找条区域（标签栏与编辑区之间）：命中自绘按钮或保持焦点在输入框
        if (g_hFind && x >= leftBar() && y >= MENU_H + TAB_H && y < MENU_H + TAB_H + FIND_H) {
            POINT pt = { x, y };
            if (PtInRect(&g_rClose, pt)) {
                g_findPress = 3;
                toggleFind();
                return 0;
            }
            if (PtInRect(&g_rPrev, pt)) {
                g_findPress = 1;
                doFind(false);
                SetFocus(g_hFind);
                InvalidateRect(hwnd, NULL, TRUE);
                return 0;
            }
            if (PtInRect(&g_rNext, pt)) {
                g_findPress = 2;
                doFind(true);
                SetFocus(g_hFind);
                InvalidateRect(hwnd, NULL, TRUE);
                return 0;
            }
            SetFocus(g_hFind);
            return 0; // 点查找条其它处：焦点留在输入框，不动文本
        }
        if (x < leftBar() + g_gutterW) return 0; // 点中行号区或侧栏，忽略
        // 编辑器内单击：清除之前的分词高亮，避免残留
        if (!g_markWord.empty()) {
            g_markWord.clear();
            g_markFlag.clear();
            g_markRanges.clear();
        }
        int ey = y - editorTop();
        int v = ey / g_lineH + g_topLine; // 由 y 反推视觉行
        if (v < 0 || v >= g_visualCount) return 0;
        const Visual& vis = g_visual[v];
        // 由 x 反推字符列：把像素偏移换算回列号（考虑换行模式与水平滚动）
        int target = (g_wrap ? linePrefixPx(vis.line, vis.col) : 0) + (x - leftBar() - g_gutterW) +
            (g_wrap ? 0 : g_scrollX);
        int col = pxToColAbs(vis.line, target);
        if (col < vis.col) col = vis.col;
        if (col > vis.col + vis.len) col = vis.col + vis.len;
        if (col < 0) col = 0;
        if (col > g_lineLen[vis.line]) col = g_lineLen[vis.line];
        int off = g_lineStart[vis.line] + col; // 最终字符偏移
        SetFocus(hwnd);
        g_anchorOff = off;
        g_caretOff = off;
        g_selStart = -1;
        g_selEnd = -1;
        findMatch();
        updateCaretPos();
        InvalidateRect(hwnd, NULL, TRUE);
        SetCapture(hwnd); // 捕获鼠标以便拖拽选择
        return 0;
    }
    case WM_MOUSEMOVE: {
        int x = (int)LOWORD(lp), y = (int)HIWORD(lp);
        RECT mrc;
        GetClientRect(hwnd, &mrc);
        // 顶部自绘菜单栏悬停态
        if (y < MENU_H) {
            int h = menuBarHit(x, mrc);
            if (h != g_menuHover) {
                g_menuHover = h;
                TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, g_hwnd, 0 };
                TrackMouseEvent(&tme);
                RECT rr = { 0, 0, mrc.right, MENU_H };
                InvalidateRect(hwnd, &rr, FALSE);
            }
            SetCursor(LoadCursor(NULL, IDC_ARROW));
            return 0;
        }
        // 正在拖动侧栏右缘调宽
        if (g_sidebarResizing) {
            int dx = x - g_sidebarResizeStartX;
            g_sidebarW = g_sidebarResizeStartW + dx;
            if (g_sidebarW < SIDEBAR_W_MIN) g_sidebarW = SIDEBAR_W_MIN;
            if (g_sidebarW > SIDEBAR_W_MAX) g_sidebarW = SIDEBAR_W_MAX;
            RECT r;
            GetClientRect(hwnd, &r);
            int maxW = r.right - 60;
            if (g_sidebarW > maxW) g_sidebarW = maxW;
            updateScroll(); // 编辑区宽度变化，重算水平滚动范围
            InvalidateRect(hwnd, NULL, TRUE);
            return 0;
        }
        // 正在拖动侧栏树滚动条：实时更新滚动位置
        if (g_treeDrag) {
            RECT r;
            GetClientRect(g_hwnd, &r);
            int sbTop = editorTop() + SIDEBAR_HEAD_H;
            int viewH = r.bottom - sbTop;
            int contentH = (int)g_treeRows.size() * SIDEBAR_ROW_H;
            if (contentH > viewH) {
                int maxScroll = contentH - viewH;
                if (maxScroll < 1) maxScroll = 1;
                int thumbH = max(20, (int)((double)viewH / contentH * viewH));
                int thumbY = y - g_treeDragGrab;
                int newScroll = (int)((double)(thumbY - sbTop) / (viewH - thumbH) * maxScroll);
                g_treeScroll = newScroll;
                clampTreeScroll();
                InvalidateRect(hwnd, NULL, TRUE);
            }
            return 0;
        }
        // 查找条按钮悬停态跟踪（仅编辑器区域内）
        if (g_hFind) {
            int h = 0;
            POINT pt = { x, y };
            if (x >= leftBar() && y >= MENU_H + TAB_H && y < MENU_H + TAB_H + FIND_H) {
                if (PtInRect(&g_rPrev, pt))
                    h = 1;
                else if (PtInRect(&g_rNext, pt))
                    h = 2;
                else if (PtInRect(&g_rClose, pt))
                    h = 3;
            }
            if (h != g_findHover) {
                g_findHover = h;
                TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, g_hwnd, 0 };
                TrackMouseEvent(&tme);
                RECT rc;
                GetClientRect(hwnd, &rc);
                RECT br = { leftBar(), MENU_H + TAB_H, rc.right, MENU_H + TAB_H + FIND_H };
                InvalidateRect(hwnd, &br, TRUE);
            }
        }
        // 悬停在侧栏右缘：显示左右调整光标（并跳过树行高亮）
        if (g_folderOpen && y >= MENU_H + TAB_H) {
            int lb = leftBar();
            if (x >= lb - 1 && x <= lb + 3) {
                SetCursor(LoadCursor(NULL, IDC_SIZEWE));
                return 0;
            }
        }
        // 侧栏悬停高亮（目录/文件行与关闭按钮）
        if (g_folderOpen && x < leftBar() && y >= editorTop()) {
            sidebarMove(x, y);
            return 0;
        }
        if (wp & MK_LBUTTON) {
            if (y < MENU_H + TAB_H) return 0;
            // 选区拖到编辑区边缘时自动水平滚动
            RECT r;
            GetClientRect(hwnd, &r);
            int lb = leftBar();
            int oldScrollX = g_scrollX; // 记录拖拽前的水平滚动量，用于判断是否需退化整屏重绘
            int maxX = g_maxLineW; // 缓存最大行宽，避免拖拽选择时每行 linePx 全量扫描（大文件卡顿主因）
            int hmax = maxX - (r.right - r.left - lb - g_gutterW);
            if (hmax < 0) hmax = 0;
            if (hmax > 0 && x >= lb + g_gutterW) {
                int step = g_charW * 3;
                bool scrolled = false;
                if (x > r.right - 24) {
                    g_scrollX += step;
                    scrolled = true;
                }
                else if (x < lb + g_gutterW + 24) {
                    g_scrollX -= step;
                    scrolled = true;
                }
                if (g_scrollX < 0) g_scrollX = 0;
                if (g_scrollX > hmax) g_scrollX = hmax;
                if (scrolled) { SetScrollPos(hwnd, SB_HORZ, g_scrollX, TRUE); }
            }
            int ey = y - editorTop();
            int v = ey / g_lineH + g_topLine;
            // 越界/空窗口早退：当 g_visualCount == 0（关闭所有标签后的空窗口）时，
            // “v = g_visualCount - 1” 会得到 -1，访问 g_visual[-1] 越界读出垃圾 vis.line，
            // 进而 pxToColAbs 内 g_lineStart[vis.line] 以 null-12 访问 → 0xC0000005。
            if (v < 0 || v >= g_visualCount) return 0;
            const Visual& vis = g_visual[v];
            int target = (g_wrap ? linePrefixPx(vis.line, vis.col) : 0) + (x - lb - g_gutterW) +
                (g_wrap ? 0 : g_scrollX);
            int col = pxToColAbs(vis.line, target);
            if (col < vis.col) col = vis.col;
            if (col > vis.col + vis.len) col = vis.col + vis.len;
            if (col < 0) col = 0;
            if (col > g_lineLen[vis.line]) col = g_lineLen[vis.line];
            int off = g_lineStart[vis.line] + col;
            g_caretOff = off;
            int oS = g_selStart, oE = g_selEnd, oMa = g_matchA, oMb = g_matchB, oMaW = g_matchAw,
                oMbW = g_matchBw;
            // 以 anchor 为起点、当前 off 为终点，生成选区 [selStart, selEnd)
            if (g_anchorOff < g_caretOff) {
                g_selStart = g_anchorOff;
                g_selEnd = g_caretOff;
            }
            else {
                g_selStart = g_caretOff;
                g_selEnd = g_anchorOff;
            }
            findMatch();
            if (g_scrollX != oldScrollX) {
                // 水平位置变化：所有可见行 x 偏移改变，退化为整屏重绘（仅拖到左右边缘时触发）
                InvalidateRect(hwnd, NULL, TRUE);
            }
            else {
                // 仅重绘“选中/匹配状态”发生变化的可见行（拖选卡顿优化核心）
                repaintSelDir(oS, oE, g_selStart, g_selEnd, oMa, oMaW, oMb, oMbW, g_matchA, g_matchAw,
                    g_matchB, g_matchBw);
            }
            updateCaretPos();
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        if (g_sidebarResizing) {
            g_sidebarResizing = false;
            ReleaseCapture();
            return 0;
        }
        if (g_treeDrag) {
            g_treeDrag = false;
            ReleaseCapture();
            return 0;
        }
        if (g_findPress) {
            g_findPress = 0;
            RECT rc;
            GetClientRect(hwnd, &rc);
            RECT br = { 0, MENU_H + TAB_H, rc.right, MENU_H + TAB_H + FIND_H };
            InvalidateRect(hwnd, &br, TRUE);
        }
        ReleaseCapture();
        return 0;
    }
    case WM_MOUSELEAVE: {
        // 鼠标离开窗口：清除查找条按钮与侧栏的悬停高亮
        if (g_menuHover != -1) {
            g_menuHover = -1;
            RECT rc;
            GetClientRect(hwnd, &rc);
            RECT rr = { 0, 0, rc.right, MENU_H };
            InvalidateRect(hwnd, &rr, FALSE);
        }
        if (g_findHover) {
            g_findHover = 0;
            RECT rc;
            GetClientRect(hwnd, &rc);
            RECT br = { leftBar(), MENU_H + TAB_H, rc.right, MENU_H + TAB_H + FIND_H };
            InvalidateRect(hwnd, &br, TRUE);
        }
        if (g_sidebarHover != -1 || g_sideHoverBtn) {
            g_sidebarHover = -1;
            g_sideHoverBtn = 0;
            InvalidateRect(hwnd, NULL, TRUE);
        }
        return 0;
    }
    case WM_LBUTTONDBLCLK: {
        // 双击：把光标位置的“整词”选中，并高亮标记文档中所有相同分词
        int x = (int)LOWORD(lp), y = (int)HIWORD(lp);
        if (y < MENU_H + TAB_H) return 0;                     // 菜单栏+标签栏不处理
        if (g_hFind && y < MENU_H + TAB_H + FIND_H) return 0; // 查找条不处理
        if (x < leftBar() + g_gutterW) return 0;
        int ey = y - editorTop();
        int v = ey / g_lineH + g_topLine;
        if (v < 0 || v >= g_visualCount) return 0;
        const Visual& vis = g_visual[v];
        int target = (g_wrap ? linePrefixPx(vis.line, vis.col) : 0) + (x - leftBar() - g_gutterW) +
            (g_wrap ? 0 : g_scrollX);
        int col = pxToColAbs(vis.line, target);
        if (col < vis.col) col = vis.col;
        if (col > vis.col + vis.len) col = vis.col + vis.len;
        if (col < 0) col = 0;
        if (col > g_lineLen[vis.line]) col = g_lineLen[vis.line];
        int off = g_lineStart[vis.line] + col;
        int ws, we;
        wordAtOffset(off, ws, we); // 扩展为整词
        std::wstring w = g_text.substr(ws, we - ws);
        // SQL 的 BEGIN / END 视为块括号：双击只选中该词本身，不做全文档高亮
        // （避免一次选中所有 BEGIN 或所有 END），并触发括号配对高亮对应块。
        bool isSqlBlock = (g_langId == L_SQL && (w == L"BEGIN" || w == L"END" || w == L"CASE"));
        if (isSqlBlock) {
            g_markWord.clear();
            g_markFlag.clear();
            g_markRanges.clear(); // 清除分词高亮
        }
        else {
            g_markWord = w; // 记录标记词
            collectMarks(); // 收集所有整词命中
        }
        g_selStart = ws;
        g_selEnd = we;
        g_anchorOff = ws;
        g_caretOff = we; // 选中该分词
        findMatch();
        updateCaretPos();
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;
    }
    case WM_KEYDOWN: {
        bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
        // Alt+F/E/V：展开自绘菜单栏的对应弹出菜单
        if (alt && !ctrl && !shift) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            if (wp == 'F') {
                showMenuPopup(0, hwnd, rc);
                return 0;
            }
            else if (wp == 'E') {
                showMenuPopup(1, hwnd, rc);
                return 0;
            }
            else if (wp == 'V') {
                showMenuPopup(2, hwnd, rc);
                return 0;
            }
        }
        // 组合键：Ctrl+O/T/W/F/C/A 与 Ctrl+Tab 切换标签
        if (ctrl) {
            if (wp == 'O') {
                if (shift) {
                    std::wstring p = openFolderDialog();
                    if (!p.empty()) openFolder(p);
                }
                else {
                    std::wstring p = openFileDialog();
                    if (!p.empty()) openInNewTab(p);
                }
                return 0;
            }
            else if (wp == 'T') {
                std::wstring p = openFileDialog();
                if (!p.empty()) openInNewTab(p);
                return 0;
            }
            else if (wp == 'W') {
                closeTab(g_active);
                return 0;
            }
            else if (wp == 'F') {
                toggleFind();
                return 0;
            }
            else if (wp == 'S') {
                saveFile();
                return 0;
            }
            else if (wp == 'C') {
                if (shift) {
                    openTerminalHere();
                    return 0;
                } // Ctrl+Shift+C：在当前文件目录打开终端
                if (g_selStart >= 0) {
                    std::wstring sub = g_text.substr(g_selStart, g_selEnd - g_selStart);
                    HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, (sub.size() + 1) * 2);
                    wchar_t* p = (wchar_t*)GlobalLock(hg);
                    wcscpy_s(p, sub.size() + 1, sub.c_str());
                    GlobalUnlock(hg);
                    OpenClipboard(hwnd);
                    EmptyClipboard();
                    SetClipboardData(CF_UNICODETEXT, hg);
                    CloseClipboard();
                }
                return 0;
            }
            else if (wp == 'X') {
                cutSelection();
                return 0;
            } // Ctrl+X 剪切
            else if (wp == 'V') {
                pasteFromClipboard();
                return 0;
            } // Ctrl+V 粘贴
            else if (wp == 'A') {
                g_selStart = 0;
                g_selEnd = (int)g_text.size();
                g_anchorOff = 0;
                g_caretOff = (int)g_text.size();
                InvalidateRect(hwnd, NULL, TRUE);
                return 0;
            }
            else if (wp == 'Z') {
                if (shift)
                    redo();
                else
                    undo();
                return 0;
            } // Ctrl+Z 撤销；Ctrl+Shift+Z 重做
            else if (wp == 'Y') {
                redo();
                return 0;
            } // Ctrl+Y 重做
            else if (wp == VK_TAB) {
                int n = (int)g_docs.size();
                if (n > 1) {
                    int nx = shift ? (g_active - 1 + n) % n : (g_active + 1) % n;
                    switchTab(nx);
                }
                return 0;
            }
        }
        // 代码补全候选列表导航（开启且可见时，拦截 ↑/↓/Tab/Enter/Esc；其余键先收起，输入键会在 WM_CHAR
        // 重新触发）
        if (g_compVisible && !ctrl && !alt) {
            if (wp == VK_DOWN) {
                g_compSel++;
                if (g_compSel >= (int)g_compItems.size()) g_compSel = 0;
                RedrawWindow(g_hComp, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
                return 0;
            }
            if (wp == VK_UP) {
                g_compSel--;
                if (g_compSel < 0) g_compSel = (int)g_compItems.size() - 1;
                RedrawWindow(g_hComp, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
                return 0;
            }
            if (wp == VK_TAB || wp == VK_RETURN) {
                acceptCompletion();
                return 0;
            }
            if (wp == VK_ESCAPE) {
                hideCompletion();
                return 0;
            }
            hideCompletion(); // 其余按键（方向/Home/End/普通字符等）先收起，避免陈旧列表
        }
        // 方向键/Home/End/PageUp/PageDown 移动光标（保持列号，遇短行则夹取）
        int line = lineOfOffset(g_caretOff);
        int col = g_caretOff - g_lineStart[line];
        int np = g_caretOff; // 新的光标偏移
        switch (wp) {
        case VK_LEFT:
            np = g_caretOff - 1;
            break;
        case VK_RIGHT:
            np = g_caretOff + 1;
            break;
        case VK_UP: {
            if (col > 0 && g_text[g_caretOff - 1] == L'\n') {
                np = g_caretOff - 1;
                break;
            }
            if (line > 0) {
                int pl = line - 1;
                int pcol = min(col, (int)g_lineLen[pl]);
                np = g_lineStart[pl] + pcol;
                if (np > 0 && g_text[np - 1] == L'\n') np--;
            }
            break;
        }
        case VK_DOWN: {
            if (col < g_lineLen[line] && g_text[g_caretOff] == L'\n') {
                np = g_caretOff + 1;
                break;
            }
            if (line < g_lineCount - 1) {
                int nl = line + 1;
                int ncol = min(col, (int)g_lineLen[nl]);
                np = g_lineStart[nl] + ncol;
                if (np > 0 && g_text[np - 1] == L'\n') np--;
            }
            break;
        }
        case VK_HOME:
            np = g_lineStart[line];
            break;
        case VK_END:
            np = g_lineStart[line] + g_lineLen[line];
            break;
        case VK_PRIOR: {
            RECT r;
            GetClientRect(hwnd, &r);
            int page = (r.bottom - editorTop()) / g_lineH;
            int v = -1;
            for (int q = 0; q < g_visualCount; q++)
                if (g_visual[q].line == line) {
                    v = q;
                    break;
                }
            v -= page;
            if (v < 0) v = 0;
            np = g_lineStart[g_visual[v].line] + min(col, (int)g_lineLen[g_visual[v].line]);
            break;
        }
        case VK_NEXT: {
            RECT r;
            GetClientRect(hwnd, &r);
            int page = (r.bottom - editorTop()) / g_lineH;
            int v = -1;
            for (int q = 0; q < g_visualCount; q++)
                if (g_visual[q].line == line) {
                    v = q;
                    break;
                }
            v += page;
            if (v >= g_visualCount) v = g_visualCount - 1;
            np = g_lineStart[g_visual[v].line] + min(col, (int)g_lineLen[g_visual[v].line]);
            break;
        }
        case VK_F3:
            doFind(true);
            return 0; // F3 重复上次查找
        case VK_F12:
            gotoDefinition(selectedOrWordAtCaret());
            return 0; // 跳转到定义
            // 轻量编辑：退格/删除/回车/制表符（均作用于编辑器文本，交给编辑函数处理并保持光标）
        case VK_BACK:
            deleteChar(false);
            if (g_autocomplete) updateCompletion();
            return 0;
        case VK_DELETE:
            deleteChar(true);
            if (g_autocomplete) updateCompletion();
            return 0;
        case VK_RETURN:
            insertText(L"\n");
            return 0;
        case VK_TAB:
            insertText(L"\t");
            return 0;
        default:
            return DefWindowProc(hwnd, msg, wp, lp); // 其它按键交还系统
        }
        if (np < 0) np = 0;
        if (np > (int)g_text.size()) np = (int)g_text.size();
        g_caretOff = np;
        if (shift) {
            if (g_anchorOff < g_caretOff) {
                g_selStart = g_anchorOff;
                g_selEnd = g_caretOff;
            }
            else {
                g_selStart = g_caretOff;
                g_selEnd = g_anchorOff;
            }
        }
        else {
            g_anchorOff = g_caretOff;
            g_selStart = -1;
            g_selEnd = -1;
        } // 无 Shift：取消选区，锚点跟随
        findMatch();
        setCaret(np);
        return 0;
    }
    case WM_SYSKEYDOWN: {
        // 拦截 Alt+F/E/V，避免被系统菜单抢走；其它系统键交给默认处理
        bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        if (!ctrl && !shift && (wp == 'F' || wp == 'E' || wp == 'V')) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            int idx = (wp == 'F') ? 0 : (wp == 'E') ? 1 : 2;
            showMenuPopup(idx, hwnd, rc);
            return 0;
        }
        return DefWindowProc(hwnd, msg, wp, lp);
    }
    case WM_CHAR: {
        wchar_t ch = (wchar_t)wp;
        if (ch == 0x7F) {
            deleteChar(true);
            if (g_autocomplete) updateCompletion();
            return 0;
        }                        // DEL
        if (ch < 0x20) return 0; // 控制字符（回车/退格/Tab 已在 WM_KEYDOWN 处理）
        if (g_hFind && GetFocus() == g_hFind)
            return DefWindowProc(hwnd, msg, wp, lp); // 焦点在查找框时不当作编辑器输入
        // 代码补全：括号/引号自动配对（开启时）
        if (g_autocomplete) {
            // 尖括号仅在“前面紧跟标识符”的模板/泛型语境下自动配对（如 Vector<），
            // 避免 a < b 这类比较运算被误配对；其余配对字符（(){}[]""''）正常处理。
            bool pair = isPairOpen(ch) &&
                !(ch == L'<' && !(g_caretOff > 0 && isWordChar(g_text[g_caretOff - 1])));
            if (pair) {
                wchar_t close = pairClose(ch);
                if (g_selStart >= 0) { // 选中区域：用配对字符包裹
                    std::wstring sel = g_text.substr(g_selStart, g_selEnd - g_selStart);
                    applyEdit(g_selStart, g_selEnd, std::wstring(1, ch) + sel + std::wstring(1, close), true);
                }
                else {
                    insertText(std::wstring(1, ch) + std::wstring(1, close)); // 成对插入
                    g_caretOff--;
                    updateCaretPos(); // 光标置于配对中间
                }
                updateCompletion();
                return 0;
            }
            // 右配对字符：若其后恰为该字符，则跳过而非重复插入
            if (isPairClose(ch) && g_caretOff < (int)g_text.size() && g_text[g_caretOff] == ch) {
                g_caretOff++;
                updateCaretPos();
                updateCompletion();
                return 0;
            }
        }
        insertText(std::wstring(1, ch));        // 普通字符：插入到光标处
        if (g_autocomplete) updateCompletion(); // 触发关键字/函数补全
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == 2001) {
            // 查找输入框：回车/ESC 已由 FindEditProc 子类拦截处理（见 toggleFind）。
            // 单行 EDIT 无 EN_RETURN 通知，故不在此依赖 0x0300（实为 EN_CHANGE 文本变更），避免误触发。
        }
        else if (id == 1001) {
            std::wstring p = openFileDialog();
            if (!p.empty()) openInNewTab(p);
        }
        else if (id == 1004) {
            std::wstring p = openFileDialog();
            if (!p.empty()) openInNewTab(p);
        }
        else if (id == 1005) {
            closeTab(g_active);
        }
        else if (id == 1006) {
            saveFile();
        }
        else if (id == 1002) {
            registerDefault();
        }
        else if (id == 1003) {
            DestroyWindow(hwnd);
        }
        else if (id == 1007) {
            std::wstring p = openFolderDialog();
            if (!p.empty()) openFolder(p);
        }
        else if (id == 1008) {
            closeFolder();
        }
        else if (id == 1101) {
            copySelection();
        } // 复制（主菜单“编辑”）
        else if (id == 1102) {
            g_selStart = 0;
            g_selEnd = (int)g_text.size();
            g_anchorOff = 0;
            g_caretOff = (int)g_text.size();
            InvalidateRect(hwnd, NULL, TRUE);
        }
        else if (id == 1103) {
            toggleFind();
        }
        // 编辑区右键菜单
        else if (id == 1401) {
            copySelection();
        }
        else if (id == 1402) {
            cutSelection();
        }
        else if (id == 1403) {
            pasteFromClipboard();
        }
        else if (id == 1404) {
            selectAll();
        }
        else if (id == 1405) {
            openTerminalAndPasteSel();
        } // 在命令提示符中打开（粘贴选中文本，不提交）
        else if (id == 1406) {
            gotoDefinition(selectedOrWordAtCaret());
        } // 跳转到定义（函数/变量）
        // 标签栏右键菜单
        else if (id == 1501) {
            if (g_ctxTab >= 0) closeLeftTabs(g_ctxTab);
        }
        else if (id == 1502) {
            if (g_ctxTab >= 0) closeRightTabs(g_ctxTab);
        }
        else if (id == 1503) {
            if (g_ctxTab >= 0) closeOtherTabs(g_ctxTab);
        }
        else if (id >= 1250 && id <= 1254) {
            setTheme(id - 1250);
            saveConfig();
        } // 切换主题并持久化
        else if (id == 1202) {
            g_wrap = !g_wrap;
            buildVisual();
            invalidateLineCache();
            updateScroll();
            InvalidateRect(hwnd, NULL, TRUE);
        }
        else if (id == 1203) {
            g_fontSize++;
            if (g_fontSize > 28) g_fontSize = 28;
            ensureFont();
            buildVisual();
            updateScroll();
            InvalidateRect(hwnd, NULL, TRUE);
            updateCaretPos();
        }
        else if (id == 1204) {
            g_fontSize--;
            if (g_fontSize < 9) g_fontSize = 9;
            ensureFont();
            buildVisual();
            updateScroll();
            InvalidateRect(hwnd, NULL, TRUE);
            updateCaretPos();
        }
        else if (id >= 1300 && id <= 1313) { // 选择语言
            const wchar_t* langs[] = { L"auto",   L"txt", L"csharp", L"sql", L"html", L"js",   L"json",
                                      L"python", L"css", L"c",      L"cpp", L"java", L"aspx", L"xml" };
            g_lang = langs[id - 1300];
            g_langId = langFromName();
            g_tokens.clear();
            g_stateDone.assign(g_lineCount, 0);
            g_stateDone[0] = 1;    // 清缓存重着色（跨行状态重新按需计算）
            invalidateLineCache(); // 着色规则变化，行缓存失效
            // 同步到当前文档快照
            if (g_active >= 0) g_docs[g_active].lang = g_lang, g_docs[g_active].langId = g_langId;
            InvalidateRect(hwnd, NULL, TRUE);
            setWindowTitle();
        }
        else if (id == 1260) {
            g_autocomplete = !g_autocomplete;
            saveConfig();
            checkMenus();
        }                 // 代码补全开关（持久化到 .ini）
        hideCompletion(); // 任意菜单命令后收起补全列表
        return 0;
    }
    case WM_CLOSE:
        saveConfig();
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

// ----------------------------------------------------------------------------
// 关键字表
// 程序启动时调用一次，把各语言的关键字/类型名灌入对应 set。
// ----------------------------------------------------------------------------
void initKeywords() {
    const wchar_t* cs[] = {
        L"abstract", L"as",        L"base",       L"break",    L"case",     L"catch",
        L"checked",  L"class",     L"const",      L"continue", L"default",  L"delegate",
        L"do",       L"else",      L"event",      L"explicit", L"extern",   L"finally",
        L"fixed",    L"for",       L"foreach",    L"goto",     L"if",       L"implicit",
        L"in",       L"interface", L"internal",   L"is",       L"lock",     L"namespace",
        L"new",      L"null",      L"operator",   L"out",      L"override", L"params",
        L"private",  L"protected", L"public",     L"readonly", L"ref",      L"return",
        L"sealed",   L"sizeof",    L"stackalloc", L"static",   L"switch",   L"this",
        L"throw",    L"true",      L"false",      L"try",      L"typeof",   L"uint",
        L"ulong",    L"unchecked", L"unsafe",     L"using",    L"virtual",  L"volatile",
        L"while",    L"add",       L"async",      L"await",    L"init",     L"record",
        L"required", L"scoped",    L"unmanaged",  L"when",     L"with",     L"yield",
        L"get",      L"set",       L"value",      L"var",      NULL };
    const wchar_t* cst[] = { L"bool",   L"byte",    L"char",   L"decimal", L"double",
                            L"float",  L"int",     L"long",   L"sbyte",   L"short",
                            L"uint",   L"ulong",   L"ushort", L"void",    L"object",
                            L"string", L"dynamic", L"nint",   L"nuint",   NULL };
    const wchar_t* sql[] = {
        L"SELECT", L"FROM",     L"WHERE",    L"INSERT",      L"UPDATE",  L"DELETE",
        L"CREATE", L"TABLE",    L"DROP",     L"ALTER",       L"ADD",     L"COLUMN",
        L"INDEX",  L"VIEW",     L"FUNCTION", L"PROCEDURE",   L"TRIGGER", L"DATABASE",
        L"SCHEMA", L"JOIN",     L"INNER",    L"LEFT",        L"RIGHT",   L"OUTER",
        L"FULL",   L"CROSS",    L"ON",       L"AS",          L"AND",     L"OR",
        L"NOT",    L"NULL",     L"IS",       L"IN",          L"LIKE",    L"BETWEEN",
        L"EXISTS", L"ANY",      L"ALL",      L"GROUP",       L"BY",      L"ORDER",
        L"HAVING", L"DISTINCT", L"UNION",    L"INTERSECT",   L"EXCEPT",  L"INTO",
        L"VALUES", L"SET",      L"PRIMARY",  L"KEY",         L"FOREIGN", L"REFERENCES",
        L"UNIQUE", L"DEFAULT",  L"CHECK",    L"CONSTRAINT",  L"CASCADE", L"CASE",
        L"WHEN",   L"THEN",     L"ELSE",     L"END",         L"LIMIT",   L"OFFSET",
        L"ASC",    L"DESC",     L"TOP",      L"WITH",        L"GRANT",   L"REVOKE",
        L"BEGIN",  L"COMMIT",   L"ROLLBACK", L"TRANSACTION", L"DECLARE", L"IF",
        L"WHILE",  L"LOOP",     L"RETURN",   L"CAST",        L"CONVERT", L"COALESCE",
        L"COUNT",  L"SUM",      L"AVG",      L"MIN",         L"MAX",     L"IDENTITY",
        L"ENGINE", L"CHARSET",  L"COLLATE",  L"EXEC",        L"EXECUTE", NULL };
    const wchar_t* js[] = {
        L"var",    L"let",   L"const",     L"function", L"return",  L"if",      L"else",
        L"for",    L"while", L"do",        L"switch",   L"case",    L"break",   L"continue",
        L"new",    L"class", L"extends",   L"super",    L"this",    L"typeof",  L"instanceof",
        L"in",     L"of",    L"try",       L"catch",    L"finally", L"throw",   L"await",
        L"async",  L"yield", L"import",    L"export",   L"from",    L"default", L"void",
        L"delete", L"null",  L"undefined", L"true",     L"false",   L"NaN",     L"arguments",
        L"get",    L"set",   L"static",    NULL };
    const wchar_t* py[] = {
        L"def",     L"class",    L"return", L"if",   L"elif",   L"else",     L"for",   L"while",
        L"break",   L"continue", L"import", L"from", L"as",     L"with",     L"try",   L"except",
        L"finally", L"raise",    L"lambda", L"pass", L"global", L"nonlocal", L"yield", L"async",
        L"await",   L"in",       L"is",     L"not",  L"and",    L"or",       L"None",  L"True",
        L"False",   L"self",     L"assert", L"del",  L"print",  NULL };
    const wchar_t* css[] = { L"@import",  L"@media", L"@keyframes", L"@font-face",
                            L"@charset", L"@page",  L"important",  NULL };
    for (int i = 0; cs[i]; i++) KW_CS.insert(cs[i]);
    for (int i = 0; cst[i]; i++) TY_CS.insert(cst[i]);
    for (int i = 0; sql[i]; i++) KW_SQL.insert(sql[i]);
    for (int i = 0; js[i]; i++) KW_JS.insert(js[i]);
    for (int i = 0; py[i]; i++) KW_PY.insert(py[i]);
    for (int i = 0; css[i]; i++) KW_CSS.insert(css[i]);

    const wchar_t* ckw[] = { L"auto",       L"break",     L"case",           L"char",
                            L"const",      L"continue",  L"default",        L"do",
                            L"double",     L"else",      L"enum",           L"extern",
                            L"float",      L"for",       L"goto",           L"if",
                            L"inline",     L"int",       L"long",           L"register",
                            L"restrict",   L"return",    L"short",          L"signed",
                            L"sizeof",     L"static",    L"struct",         L"switch",
                            L"typedef",    L"union",     L"unsigned",       L"void",
                            L"volatile",   L"while",     L"_Alignas",       L"_Alignof",
                            L"_Atomic",    L"_Bool",     L"_Complex",       L"_Generic",
                            L"_Imaginary", L"_Noreturn", L"_Static_assert", L"_Thread_local",
                            NULL };
    const wchar_t* cppkw[] = { L"asm",
                              L"auto",
                              L"bool",
                              L"break",
                              L"case",
                              L"catch",
                              L"char",
                              L"class",
                              L"const",
                              L"const_cast",
                              L"constexpr",
                              L"continue",
                              L"decltype",
                              L"default",
                              L"delete",
                              L"do",
                              L"double",
                              L"dynamic_cast",
                              L"else",
                              L"enum",
                              L"explicit",
                              L"export",
                              L"extern",
                              L"false",
                              L"float",
                              L"for",
                              L"friend",
                              L"goto",
                              L"if",
                              L"inline",
                              L"int",
                              L"long",
                              L"mutable",
                              L"namespace",
                              L"new",
                              L"noexcept",
                              L"nullptr",
                              L"operator",
                              L"private",
                              L"protected",
                              L"public",
                              L"register",
                              L"reinterpret_cast",
                              L"return",
                              L"short",
                              L"signed",
                              L"sizeof",
                              L"static",
                              L"static_assert",
                              L"static_cast",
                              L"struct",
                              L"switch",
                              L"template",
                              L"this",
                              L"throw",
                              L"true",
                              L"try",
                              L"typedef",
                              L"typeid",
                              L"typename",
                              L"union",
                              L"unsigned",
                              L"using",
                              L"virtual",
                              L"void",
                              L"volatile",
                              L"wchar_t",
                              L"while",
                              L"and",
                              L"and_eq",
                              L"bitand",
                              L"bitor",
                              L"compl",
                              L"not",
                              L"not_eq",
                              L"or",
                              L"or_eq",
                              L"xor",
                              L"xor_eq",
                              L"final",
                              L"override",
                              L"concept",
                              L"requires",
                              L"co_await",
                              L"co_return",
                              L"co_yield",
                              L"import",
                              L"module",
                              L"char8_t",
                              L"char16_t",
                              L"char32_t",
                              L"thread_local",
                              NULL };
    const wchar_t* javakw[] = { L"abstract",   L"assert",       L"boolean",   L"break",      L"byte",
                               L"case",       L"catch",        L"char",      L"class",      L"const",
                               L"continue",   L"default",      L"do",        L"double",     L"else",
                               L"enum",       L"extends",      L"final",     L"finally",    L"float",
                               L"for",        L"goto",         L"if",        L"implements", L"import",
                               L"instanceof", L"int",          L"interface", L"long",       L"native",
                               L"new",        L"package",      L"private",   L"protected",  L"public",
                               L"return",     L"short",        L"static",    L"strictfp",   L"super",
                               L"switch",     L"synchronized", L"this",      L"throw",      L"throws",
                               L"transient",  L"try",          L"void",      L"volatile",   L"while",
                               L"var",        L"true",         L"false",     L"null",       NULL };
    const wchar_t* cty[] = {
        L"int",      L"char",      L"float",    L"double",   L"void",     L"long",    L"short",
        L"unsigned", L"signed",    L"size_t",   L"ssize_t",  L"int8_t",   L"int16_t", L"int32_t",
        L"int64_t",  L"uint8_t",   L"uint16_t", L"uint32_t", L"uint64_t", L"wchar_t", L"ptrdiff_t",
        L"intptr_t", L"uintptr_t", L"FILE",     L"bool",     L"_Bool",    NULL };
    const wchar_t* cppty[] = {
        L"bool",       L"wchar_t",       L"char8_t",  L"char16_t", L"char32_t",
        L"short",      L"int",           L"long",     L"float",    L"double",
        L"void",       L"size_t",        L"int8_t",   L"int16_t",  L"int32_t",
        L"int64_t",    L"uint8_t",       L"uint16_t", L"uint32_t", L"uint64_t",
        L"string",     L"wstring",       L"vector",   L"map",      L"unordered_map",
        L"set",        L"unordered_set", L"list",     L"deque",    L"queue",
        L"stack",      L"array",         L"pair",     L"tuple",    L"shared_ptr",
        L"unique_ptr", L"weak_ptr",      L"bitset",   L"ostream",  L"istream",
        L"iostream",   L"ifstream",      L"ofstream", L"cout",     L"cin",
        L"cerr",       L"endl",          L"nullptr",  NULL };
    const wchar_t* javaty[] = {
        L"int",          L"char",    L"boolean",    L"byte",      L"short",
        L"long",         L"float",   L"double",     L"void",      L"String",
        L"Object",       L"Integer", L"Boolean",    L"Double",    L"Float",
        L"Long",         L"Short",   L"Byte",       L"Character", L"StringBuilder",
        L"StringBuffer", L"List",    L"Map",        L"Set",       L"ArrayList",
        L"HashMap",      L"HashSet", L"LinkedList", L"Exception", L"RuntimeException",
        L"Throwable",    NULL };
    for (int i = 0; ckw[i]; i++) KW_C.insert(ckw[i]);
    for (int i = 0; cppkw[i]; i++) KW_CPP.insert(cppkw[i]);
    for (int i = 0; javakw[i]; i++) KW_JAVA.insert(javakw[i]);
    for (int i = 0; cty[i]; i++) TY_C.insert(cty[i]);
    for (int i = 0; cppty[i]; i++) TY_CPP.insert(cppty[i]);
    for (int i = 0; javaty[i]; i++) TY_JAVA.insert(javaty[i]);
}

// ----------------------------------------------------------------------------
// 图标：直接使用 @icon_gen.exe 生成的 appicon.ico 资源（编译进 exe 的 IDI_APPICON），
// 不再运行时用 GDI 自绘 “LR” 字标。这样 exe 文件图标与窗口标题栏/任务栏图标一致，
// 且无需在 cpp 内维护绘制逻辑。
// 注：appicon.ico 由 icon_gen.cpp 生成，经 appicon.rc 编译为资源；构建时务必先 windres。
// ----------------------------------------------------------------------------
// IDI_APPICON 必须与 appicon.rc 中的定义保持一致（当前为 101）。
#ifndef IDI_APPICON
#define IDI_APPICON 101
#endif

// 为窗口设置大/小两套图标（标题栏与任务栏显示 “LR”）。
// 大图标按 256px 取出，小图标按 32px 取出（由 Windows 按需下采样到 16/24）。
void setAppIcon(HWND hw) {
    HINSTANCE hInst = GetModuleHandle(NULL);
    HICON bigIcon =
        (HICON)LoadImage(hInst, MAKEINTRESOURCE(IDI_APPICON), IMAGE_ICON, 256, 256, LR_DEFAULTCOLOR);
    HICON smallIcon =
        (HICON)LoadImage(hInst, MAKEINTRESOURCE(IDI_APPICON), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR);
    if (bigIcon) SendMessage(hw, WM_SETICON, ICON_BIG, (LPARAM)bigIcon);
    if (smallIcon) SendMessage(hw, WM_SETICON, ICON_SMALL, (LPARAM)smallIcon);
}

// ----------------------------------------------------------------------------
// 入口
// ----------------------------------------------------------------------------
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow) {
    g_hInst = hInstance;
    CoInitializeEx(NULL,
        COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE); // 供 SHBrowseForFolder 使用
    initKeywords();                                                    // 先灌入关键字表
    loadConfig(); // 读入上次配置：主题 / 字号 / 侧栏宽 / 窗口布局 / 上次文件夹

    // ---- 单实例：若已存在窗口，把命令行文件发给它并激活，不另开进程 ----
    HANDLE hMutex = CreateMutexW(NULL, TRUE, L"LiteReaderSingleInstance_v1");
    bool other = (GetLastError() == ERROR_ALREADY_EXISTS);
    if (other) {
        HWND hw = NULL;
        for (int t = 0; t < 60 && !hw; t++) {
            hw = FindWindowW(WNDCLASS_NAME, NULL);
            if (!hw) Sleep(20);
        } // 轮询等待目标窗口出现
        if (hw) {
            int argc;
            LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
            for (int a = 1; a < argc; a++) {
                COPYDATASTRUCT cds;
                cds.dwData = 1;
                cds.cbData = (UINT)((wcslen(argv[a]) + 1) * sizeof(wchar_t));
                cds.lpData = (PVOID)argv[a];
                SendMessageW(hw, WM_COPYDATA, (WPARAM)NULL,
                    (LPARAM)&cds); // 通过 WM_COPYDATA 把路径传给已有实例
            }
            if (argv) LocalFree(argv);
            bringToFront(hw);
            CloseHandle(hMutex);
            CoUninitialize();
            return 0; // 自己退出，由已有实例打开文件
        }
    }

    WNDCLASS wc = { 0 };
    wc.style = CS_DBLCLKS; // 启用双击消息（WM_LBUTTONDBLCLK），用于双击选词/高亮
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_IBEAM);
    wc.lpszClassName = WNDCLASS_NAME;
    wc.hbrBackground = CreateSolidBrush(TH.bg); // 用主题背景色，避免窗口拉伸/非客户区出现白闪
    RegisterClass(&wc);
    g_hwnd = CreateWindowEx(WS_EX_ACCEPTFILES, WNDCLASS_NAME, L"LiteReader",
        WS_OVERLAPPEDWINDOW | WS_VSCROLL | WS_HSCROLL, CW_USEDEFAULT,
        CW_USEDEFAULT, 900, 640, NULL, NULL, hInstance, NULL);
    createMenus();      // 创建自绘菜单栏的三个弹出菜单（不再挂系统菜单条）
    setAppIcon(g_hwnd); // 加载 appicon.ico 资源中的 “LR” 图标（由 @icon_gen.exe 生成，经 appicon.rc 编译）

    // 在窗口显示前先把非客户区（标题栏、边框）设为当前主题色
    applyThemeToFrame();

    // 恢复上次窗口布局（位置/大小，以及是否最大化）
    if (g_cfgMax == 1) {
        ShowWindow(g_hwnd, SW_SHOWMAXIMIZED);
    }
    else if (g_cfgW > 0 && g_cfgH > 0) {
        SetWindowPos(g_hwnd, NULL, g_cfgX, g_cfgY, g_cfgW, g_cfgH, SWP_NOZORDER | SWP_NOACTIVATE);
        ShowWindow(g_hwnd, nCmdShow);
    }
    else {
        ShowWindow(g_hwnd, nCmdShow);
    }
    UpdateWindow(g_hwnd);
    applyThemeToFrame(); // 让窗口边框/标题栏颜色匹配当前主题

    // 重新打开上次文件夹（侧栏）
    if (!g_cfgFolder.empty()) openFolder(g_cfgFolder);

    // 命令行参数 = 要打开的文件（载入首个标签）
    int argc;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc >= 2) {
        loadFile(argv[1]);
        g_docs[0].filePath = g_filePath;
    }
    else {
        setWindowTitle();
    }
    if (argv) LocalFree(argv);

    // 标准 Win32 消息循环
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    CoUninitialize();
    return 0;
}
