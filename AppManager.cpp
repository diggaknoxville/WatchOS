#include "AppManager.h"
#include "Globals.h"
#include "Display.h"
#include "Logic.h"
#include "HardwareConfig.h"
#include "NotificationManager.h"
#include "LoraManager.h"
#include <FFat.h>
#include <algorithm>
#include <map>
#include <math.h>

extern void playSynthSound(int type, int duration);

static std::vector<AppPackage> registry;

// ============================================================================
// WatchJS: Leichtgewichtige JavaScript-Engine für dynamische .app Pakete
// ============================================================================
struct JsVal {
    bool isStr = false;
    double num = 0.0;
    String str = "";

    JsVal() : isStr(false), num(0.0), str("") {}
    JsVal(double n) : isStr(false), num(n), str("") {}
    JsVal(int n) : isStr(false), num((double)n), str("") {}
    JsVal(const String &s) : isStr(true), num(0.0), str(s) {}
    JsVal(const char *s) : isStr(true), num(0.0), str(s ? s : "") {}

    double asNum() const {
        if (!isStr) return num;
        if (str.length() == 0) return 0.0;
        return str.toDouble();
    }

    String asStr() const {
        if (isStr) return str;
        long iv = (long)num;
        if (fabs(num - (double)iv) < 0.00001) return String(iv);
        char buf[24];
        snprintf(buf, sizeof(buf), "%.2f", num);
        return String(buf);
    }

    bool asBool() const {
        if (isStr) return str.length() > 0 && str != "0" && !str.equalsIgnoreCase("false");
        return fabs(num) > 0.000001;
    }
};

static std::map<String, std::map<String, JsVal>> appStates;

int AppManager::prodMenuPage = 0;
int AppManager::secMenuPage = 0;
int AppManager::goodMenuPage = 0;
int AppManager::badMenuPage = 0;

static std::vector<String> getUninstalledList() {
    std::vector<String> list;
    if (!littleFsReady || !FFat.exists("/apps/uninstalled.txt")) return list;
    fs::File f = FFat.open("/apps/uninstalled.txt", "r");
    if (!f) return list;
    String cur = "";
    while (f.available()) {
        char c = (char)f.read();
        if (c == '\n' || c == '\r') {
            cur.trim();
            if (cur.length() > 0) list.push_back(cur);
            cur = "";
        } else {
            cur += c;
        }
    }
    cur.trim();
    if (cur.length() > 0) list.push_back(cur);
    f.close();
    return list;
}

static void saveUninstalledList(const std::vector<String>& list) {
    if (!littleFsReady) return;
    if (!FFat.exists("/apps")) FFat.mkdir("/apps");
    fs::File f = FFat.open("/apps/uninstalled.txt", "w");
    if (!f) return;
    for (const auto& id : list) {
        f.println(id);
    }
    f.close();
}

static uint16_t parseHexColor(const String &hex) {
    if (hex.length() < 7 || hex.charAt(0) != '#') return 0x07FF;
    long rgb = strtol(hex.substring(1).c_str(), NULL, 16);
    uint8_t r = (rgb >> 16) & 0xFF;
    uint8_t g = (rgb >> 8) & 0xFF;
    uint8_t b = rgb & 0xFF;
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

// ============================================================================
// JSON Hilfsfunktionen (mit Escape- und String-Schutz)
// ============================================================================
static int findMatchingBracket(const String &s, int openPos) {
    if (openPos < 0 || openPos >= (int)s.length()) return -1;
    char openChar = s.charAt(openPos);
    char closeChar = (openChar == '{') ? '}' : (openChar == '[' ? ']' : (openChar == '(' ? ')' : 0));
    if (!closeChar) return -1;
    int depth = 0;
    bool inStr = false;
    char quoteCh = 0;
    for (int i = openPos; i < (int)s.length(); i++) {
        char c = s.charAt(i);
        if (inStr) {
            if (c == '\\') { i++; continue; }
            if (c == quoteCh) inStr = false;
            continue;
        }
        if (c == '"' || c == '\'') {
            inStr = true;
            quoteCh = c;
            continue;
        }
        if (c == openChar) depth++;
        else if (c == closeChar) {
            depth--;
            if (depth == 0) return i;
        }
    }
    return -1;
}

static String jsonExtractString(const String &jsonStr, const String &key, const String &dflt = "") {
    String search = "\"" + key + "\"";
    int searchFrom = 0;
    while (searchFrom < (int)jsonStr.length()) {
        int idx = jsonStr.indexOf(search, searchFrom);
        if (idx == -1) return dflt;
        int colon = jsonStr.indexOf(':', idx + search.length());
        if (colon == -1) return dflt;
        bool onlyWs = true;
        for (int i = idx + (int)search.length(); i < colon; i++) {
            char c = jsonStr.charAt(i);
            if (c != ' ' && c != '\t' && c != '\r' && c != '\n') { onlyWs = false; break; }
        }
        if (!onlyWs) { searchFrom = idx + search.length(); continue; }

        int p = colon + 1;
        while (p < (int)jsonStr.length() && (jsonStr.charAt(p) == ' ' || jsonStr.charAt(p) == '\t' || jsonStr.charAt(p) == '\r' || jsonStr.charAt(p) == '\n')) p++;
        if (p >= (int)jsonStr.length() || jsonStr.charAt(p) != '"') {
            searchFrom = colon + 1;
            continue;
        }
        String out = "";
        for (int i = p + 1; i < (int)jsonStr.length(); i++) {
            char c = jsonStr.charAt(i);
            if (c == '\\' && i + 1 < (int)jsonStr.length()) {
                char nxt = jsonStr.charAt(++i);
                if (nxt == 'n') out += '\n';
                else if (nxt == 'r') out += '\r';
                else if (nxt == 't') out += '\t';
                else if (nxt == '"') out += '"';
                else if (nxt == '\\') out += '\\';
                else out += nxt;
            } else if (c == '"') {
                return out;
            } else {
                out += c;
            }
        }
        return out;
    }
    return dflt;
}

static int jsonExtractInt(const String &jsonStr, const String &key, int dflt = 0) {
    String search = "\"" + key + "\"";
    int idx = jsonStr.indexOf(search);
    if (idx == -1) return dflt;
    int colon = jsonStr.indexOf(':', idx + search.length());
    if (colon == -1) return dflt;
    int start = colon + 1;
    while (start < (int)jsonStr.length() && (jsonStr.charAt(start) == ' ' || jsonStr.charAt(start) == '\t' || jsonStr.charAt(start) == '\r' || jsonStr.charAt(start) == '\n')) start++;
    int end = start;
    while (end < (int)jsonStr.length() && (isDigit(jsonStr.charAt(end)) || jsonStr.charAt(end) == '-')) end++;
    if (start == end) return dflt;
    return (int)jsonStr.substring(start, end).toInt();
}

static String jsonExtractBlock(const String &jsonStr, const String &key) {
    String search = "\"" + key + "\"";
    int idx = jsonStr.indexOf(search);
    if (idx == -1) return "";
    int colon = jsonStr.indexOf(':', idx + search.length());
    if (colon == -1) return "";
    int start = colon + 1;
    while (start < (int)jsonStr.length() && (jsonStr.charAt(start) == ' ' || jsonStr.charAt(start) == '\t' || jsonStr.charAt(start) == '\r' || jsonStr.charAt(start) == '\n')) start++;
    if (start < (int)jsonStr.length() && (jsonStr.charAt(start) == '{' || jsonStr.charAt(start) == '[')) {
        int end = findMatchingBracket(jsonStr, start);
        if (end != -1) return jsonStr.substring(start, end + 1);
    }
    return "";
}

// ============================================================================
// WatchJS Interpreter Implementierung
// ============================================================================
static String stripJsComments(const String &code) {
    String out = "";
    out.reserve(code.length());
    bool inStr = false;
    char q = 0;
    for (int i = 0; i < (int)code.length(); i++) {
        char c = code.charAt(i);
        if (inStr) {
            out += c;
            if (c == '\\' && i + 1 < (int)code.length()) { out += code.charAt(++i); continue; }
            if (c == q) inStr = false;
            continue;
        }
        if (c == '"' || c == '\'') {
            inStr = true;
            q = c;
            out += c;
            continue;
        }
        if (c == '/' && i + 1 < (int)code.length()) {
            if (code.charAt(i + 1) == '/') {
                while (i < (int)code.length() && code.charAt(i) != '\n') i++;
                out += '\n';
                continue;
            } else if (code.charAt(i + 1) == '*') {
                i += 2;
                while (i + 1 < (int)code.length() && !(code.charAt(i) == '*' && code.charAt(i + 1) == '/')) i++;
                i++;
                continue;
            }
        }
        out += c;
    }
    return out;
}

static void parseInitialStateFromLogic(const String &appId, const String &logicJson) {
    String stateBlock = jsonExtractBlock(logicJson, "state");
    if (stateBlock.length() < 2) return;
    auto &st = appStates[appId];
    int i = 1;
    while (i < (int)stateBlock.length() - 1) {
        int q1 = stateBlock.indexOf('"', i);
        if (q1 == -1) break;
        int q2 = stateBlock.indexOf('"', q1 + 1);
        if (q2 == -1) break;
        String k = stateBlock.substring(q1 + 1, q2);
        int colon = stateBlock.indexOf(':', q2 + 1);
        if (colon == -1) break;
        int vStart = colon + 1;
        while (vStart < (int)stateBlock.length() && (stateBlock.charAt(vStart) == ' ' || stateBlock.charAt(vStart) == '\t' || stateBlock.charAt(vStart) == '\r' || stateBlock.charAt(vStart) == '\n')) vStart++;
        if (vStart >= (int)stateBlock.length()) break;
        if (stateBlock.charAt(vStart) == '"') {
            String val = "";
            int p = vStart + 1;
            for (; p < (int)stateBlock.length(); p++) {
                char c = stateBlock.charAt(p);
                if (c == '\\' && p + 1 < (int)stateBlock.length()) {
                    char nxt = stateBlock.charAt(++p);
                    if (nxt == 'n') val += '\n';
                    else val += nxt;
                } else if (c == '"') {
                    p++;
                    break;
                } else {
                    val += c;
                }
            }
            st[k] = JsVal(val);
            i = p;
        } else {
            int vEnd = vStart;
            while (vEnd < (int)stateBlock.length() && stateBlock.charAt(vEnd) != ',' && stateBlock.charAt(vEnd) != '}') vEnd++;
            String raw = stateBlock.substring(vStart, vEnd);
            raw.trim();
            if (raw.equalsIgnoreCase("true")) st[k] = JsVal(1.0);
            else if (raw.equalsIgnoreCase("false")) st[k] = JsVal(0.0);
            else st[k] = JsVal(raw.toDouble());
            i = vEnd + 1;
        }
    }
}

static void syncSystemGlobalsFromState(AppPackage *app) {
    if (!app) return;
    auto &st = appStates[app->id];
    bool isRun = st.count("running") ? st["running"].asBool() : false;
    if (app->id == "stopwatch" || app->driver == "stopwatch") {
        swRunning = isRun;
    } else if (app->id == "timer" || app->driver == "timer") {
        timerRunning = isRun;
    } else if (app->id == "jiggler" || app->driver == "jiggler") {
        jigglerActive = isRun;
    }
}

// Sicherer Mini-Arithmetik-Auswerter für Watch.evalCalc("12+34*2")
static double evalSimpleMathExpr(const String &expr, bool &ok) {
    ok = true;
    std::vector<double> nums;
    std::vector<char> ops;
    int i = 0;
    int n = expr.length();
    bool expectNum = true;
    while (i < n) {
        char c = expr.charAt(i);
        if (c == ' ') { i++; continue; }
        if (expectNum && (isDigit(c) || c == '.' || c == '-' || c == '+')) {
            int start = i;
            if (c == '-' || c == '+') i++;
            while (i < n && (isDigit(expr.charAt(i)) || expr.charAt(i) == '.')) i++;
            nums.push_back(expr.substring(start, i).toDouble());
            expectNum = false;
        } else if (!expectNum && (c == '+' || c == '-' || c == '*' || c == '/')) {
            while (!ops.empty()) {
                char top = ops.back();
                int p1 = (top == '*' || top == '/') ? 2 : 1;
                int p2 = (c == '*' || c == '/') ? 2 : 1;
                if (p1 >= p2 && nums.size() >= 2) {
                    double b = nums.back(); nums.pop_back();
                    double a = nums.back(); nums.pop_back();
                    ops.pop_back();
                    if (top == '+') nums.push_back(a + b);
                    else if (top == '-') nums.push_back(a - b);
                    else if (top == '*') nums.push_back(a * b);
                    else if (top == '/') {
                        if (fabs(b) < 1e-9) { ok = false; return 0.0; }
                        nums.push_back(a / b);
                    }
                } else break;
            }
            ops.push_back(c);
            expectNum = true;
            i++;
        } else {
            ok = false;
            return 0.0;
        }
    }
    while (!ops.empty() && nums.size() >= 2) {
        char top = ops.back(); ops.pop_back();
        double b = nums.back(); nums.pop_back();
        double a = nums.back(); nums.pop_back();
        if (top == '+') nums.push_back(a + b);
        else if (top == '-') nums.push_back(a - b);
        else if (top == '*') nums.push_back(a * b);
        else if (top == '/') {
            if (fabs(b) < 1e-9) { ok = false; return 0.0; }
            nums.push_back(a / b);
        }
    }
    return nums.empty() ? 0.0 : nums.back();
}

struct JsVM {
    AppPackage *app;
    String script;
    std::map<String, JsVal> &state;
    std::map<String, JsVal> locals;
    bool hasReturned = false;
    JsVal returnVal;
    int depth = 0;

    JsVM(AppPackage *a, const String &s) : app(a), script(s), state(appStates[a->id]) {}

    static int findTopLevel(const String &expr, const char *op, bool rightToLeft = false) {
        int opLen = strlen(op);
        int paren = 0;
        bool inStr = false;
        char q = 0;
        int found = -1;
        for (int i = 0; i <= (int)expr.length() - opLen; i++) {
            char c = expr.charAt(i);
            if (inStr) {
                if (c == '\\') { i++; continue; }
                if (c == q) inStr = false;
                continue;
            }
            if (c == '"' || c == '\'') { inStr = true; q = c; continue; }
            if (c == '(' || c == '[' || c == '{') { paren++; continue; }
            if (c == ')' || c == ']' || c == '}') { paren--; continue; }
            if (paren == 0 && expr.substring(i, i + opLen) == op) {
                // Vermeide Teil-Treffer (z.B. '=' in '==' oder '!=' oder '<=' oder '>=')
                if (opLen == 1 && (op[0] == '=' || op[0] == '<' || op[0] == '>' || op[0] == '!')) {
                    char prev = (i > 0) ? expr.charAt(i - 1) : 0;
                    char next = (i + 1 < (int)expr.length()) ? expr.charAt(i + 1) : 0;
                    if (next == '=' || prev == '=' || prev == '!' || prev == '<' || prev == '>') continue;
                }
                if (opLen == 1 && (op[0] == '+' || op[0] == '-')) {
                    // Unäres Vorzeichen am Anfang oder nach Operator ignorieren
                    int p = i - 1;
                    while (p >= 0 && expr.charAt(p) == ' ') p--;
                    if (p < 0) continue;
                    char pc = expr.charAt(p);
                    if (pc == '+' || pc == '-' || pc == '*' || pc == '/' || pc == '%' || pc == '=' || pc == '<' || pc == '>' || pc == '!' || pc == '&' || pc == '|' || pc == '?' || pc == ':' || pc == ',') continue;
                }
                if (!rightToLeft) return i;
                found = i;
            }
        }
        return found;
    }

    std::vector<JsVal> evalArgs(const String &argsStr) {
        std::vector<JsVal> args;
        String s = argsStr;
        s.trim();
        if (s.length() == 0) return args;
        int start = 0, paren = 0;
        bool inStr = false;
        char q = 0;
        for (int i = 0; i <= (int)s.length(); i++) {
            char c = (i < (int)s.length()) ? s.charAt(i) : ',';
            if (inStr) {
                if (c == '\\') { i++; continue; }
                if (c == q) inStr = false;
                continue;
            }
            if (c == '"' || c == '\'') { inStr = true; q = c; continue; }
            if (c == '(' || c == '[' || c == '{') paren++;
            else if (c == ')' || c == ']' || c == '}') paren--;
            else if (c == ',' && paren == 0) {
                String part = s.substring(start, i);
                args.push_back(evalExpr(part));
                start = i + 1;
            }
        }
        return args;
    }

    JsVal callFunction(const String &fnName, const std::vector<JsVal> &args) {
        if (depth > 8) return JsVal(0);
        // 1. Watch.* Hardware & System API
        if (fnName == "Watch.millis") return JsVal((double)millis());
        if (fnName == "Watch.vibrate") { triggerVibration(); return JsVal(1); }
        if (fnName == "Watch.alarm") { triggerAlarm(); return JsVal(1); }
        if (fnName == "Watch.toast") {
            if (!args.empty()) showToast(args[0].asStr(), args.size() > 1 ? (int)args[1].asNum() : 2500);
            return JsVal(1);
        }
        if (fnName == "Watch.notify") {
            if (args.size() >= 2) NotificationManager::add(args[0].asStr().c_str(), args[1].asStr().c_str());
            return JsVal(1);
        }
        if (fnName == "Watch.sound") {
            if (args.size() >= 2) playSynthSound((int)args[0].asNum(), (int)args[1].asNum());
            return JsVal(1);
        }
        if (fnName == "Watch.formatTimeMs") {
            long ms = args.empty() ? 0 : (long)max(0.0, args[0].asNum());
            char buf[24];
            snprintf(buf, sizeof(buf), "%02d:%02d.%02d", (int)(ms / 60000), (int)((ms / 1000) % 60), (int)((ms % 1000) / 10));
            return JsVal(String(buf));
        }
        if (fnName == "Watch.formatTimeSec") {
            long sec = args.empty() ? 0 : (long)max(0.0, args[0].asNum());
            char buf[24];
            snprintf(buf, sizeof(buf), "%02d:%02d", (int)(sec / 60), (int)(sec % 60));
            return JsVal(String(buf));
        }
        if (fnName == "Watch.random") {
            long mn = args.size() >= 1 ? (long)args[0].asNum() : 0;
            long mx = args.size() >= 2 ? (long)args[1].asNum() : 100;
            if (mx <= mn) return JsVal((double)mn);
            return JsVal((double)random(mn, mx));
        }
        if (fnName == "Watch.getBattery") return JsVal((double)getBattery());
        if (fnName == "Watch.getBattVolt") return JsVal((double)getBatteryVoltageCached(false));
        if (fnName == "Watch.getSteps") return JsVal((double)stepCount);
        if (fnName == "Watch.getDecibels") return JsVal((double)getDecibels());
        if (fnName == "Watch.jiggle") {
            int dx = args.size() >= 1 ? (int)args[0].asNum() : 2;
            int dy = args.size() >= 2 ? (int)args[1].asNum() : 0;
            if (usbHidActive) Mouse.move(dx, dy);
            return JsVal(1);
        }
        if (fnName == "Watch.loraSend") {
            if (!args.empty()) LoraManager::sendMessage(args[0].asStr());
            return JsVal(1);
        }
        if (fnName == "Watch.back") {
            if (app && app->category == CAT_PRODUCTIVITY) slideToPage(Pages::GoodAppsMenu, -1, 0);
            else slideToPage(Pages::BadAppsMenu, -1, 0);
            return JsVal(1);
        }
        if (fnName == "Watch.evalCalc") {
            if (args.empty()) return JsVal("0");
            bool ok = false;
            double v = evalSimpleMathExpr(args[0].asStr(), ok);
            if (!ok) return JsVal("Error");
            return JsVal(v);
        }

        // 2. Math.* & Global Built-ins
        if (fnName == "Math.floor") return JsVal(args.empty() ? 0.0 : floor(args[0].asNum()));
        if (fnName == "Math.ceil") return JsVal(args.empty() ? 0.0 : ceil(args[0].asNum()));
        if (fnName == "Math.round") return JsVal(args.empty() ? 0.0 : round(args[0].asNum()));
        if (fnName == "Math.abs") return JsVal(args.empty() ? 0.0 : fabs(args[0].asNum()));
        if (fnName == "Math.sqrt") return JsVal(args.empty() ? 0.0 : sqrt(max(0.0, args[0].asNum())));
        if (fnName == "Math.min") return JsVal(args.size() >= 2 ? min(args[0].asNum(), args[1].asNum()) : 0.0);
        if (fnName == "Math.max") return JsVal(args.size() >= 2 ? max(args[0].asNum(), args[1].asNum()) : 0.0);
        if (fnName == "Math.random") return JsVal((double)random(0, 10000) / 10000.0);
        if (fnName == "parseInt") return JsVal(args.empty() ? 0.0 : (double)((long)args[0].asNum()));
        if (fnName == "parseFloat" || fnName == "Number") return JsVal(args.empty() ? 0.0 : args[0].asNum());
        if (fnName == "String") return JsVal(args.empty() ? String("") : args[0].asStr());

        // 3. Benutzerdefinierte Funktion im JS-Skript suchen: "function <fnName>("
        String pattern = "function " + fnName;
        int searchPos = 0;
        while (searchPos < (int)script.length()) {
            int fIdx = script.indexOf(pattern, searchPos);
            if (fIdx == -1) break;
            int afterName = fIdx + pattern.length();
            while (afterName < (int)script.length() && script.charAt(afterName) == ' ') afterName++;
            if (afterName < (int)script.length() && script.charAt(afterName) == '(') {
                int pEnd = findMatchingBracket(script, afterName);
                if (pEnd == -1) break;
                String paramList = script.substring(afterName + 1, pEnd);
                int bStart = script.indexOf('{', pEnd);
                if (bStart == -1) break;
                int bEnd = findMatchingBracket(script, bStart);
                if (bEnd == -1) break;

                auto savedLocals = locals;
                bool savedRet = hasReturned;
                JsVal savedRetVal = returnVal;

                // Parameter binden
                int pIdx = 0, pStart = 0;
                paramList.trim();
                if (paramList.length() > 0) {
                    for (int i = 0; i <= (int)paramList.length(); i++) {
                        if (i == (int)paramList.length() || paramList.charAt(i) == ',') {
                            String pName = paramList.substring(pStart, i);
                            pName.trim();
                            if (pName.length() > 0) {
                                locals[pName] = (pIdx < (int)args.size()) ? args[pIdx] : JsVal(0);
                                pIdx++;
                            }
                            pStart = i + 1;
                        }
                    }
                }

                hasReturned = false;
                returnVal = JsVal(0);
                depth++;
                execBlock(script.substring(bStart + 1, bEnd));
                depth--;

                JsVal res = returnVal;
                locals = savedLocals;
                hasReturned = savedRet;
                returnVal = savedRetVal;
                return res;
            }
            searchPos = afterName;
        }
        return JsVal(0);
    }

    JsVal evalExpr(String expr) {
        expr.trim();
        if (expr.length() == 0) return JsVal(0);

        // Äußere Klammern entfernen, sofern sie den gesamten Ausdruck umschließen
        while (expr.length() >= 2 && expr.charAt(0) == '(' && findMatchingBracket(expr, 0) == (int)expr.length() - 1) {
            expr = expr.substring(1, expr.length() - 1);
            expr.trim();
        }

        // 1. Ternärer Operator: cond ? a : b
        int qIdx = findTopLevel(expr, "?");
        if (qIdx != -1) {
            int cIdx = findTopLevel(expr.substring(qIdx + 1), ":");
            if (cIdx != -1) {
                cIdx += (qIdx + 1);
                JsVal cond = evalExpr(expr.substring(0, qIdx));
                return cond.asBool() ? evalExpr(expr.substring(qIdx + 1, cIdx)) : evalExpr(expr.substring(cIdx + 1));
            }
        }

        // 2. Logisches ODER / UND
        int orIdx = findTopLevel(expr, "||");
        if (orIdx != -1) {
            return JsVal((evalExpr(expr.substring(0, orIdx)).asBool() || evalExpr(expr.substring(orIdx + 2)).asBool()) ? 1 : 0);
        }
        int andIdx = findTopLevel(expr, "&&");
        if (andIdx != -1) {
            return JsVal((evalExpr(expr.substring(0, andIdx)).asBool() && evalExpr(expr.substring(andIdx + 2)).asBool()) ? 1 : 0);
        }

        // 3. Vergleiche
        const char* cmpOps[] = { "===", "!==", "==", "!=", "<=", ">=", "<", ">" };
        for (const char* op : cmpOps) {
            int idx = findTopLevel(expr, op);
            if (idx != -1) {
                int len = strlen(op);
                JsVal l = evalExpr(expr.substring(0, idx));
                JsVal r = evalExpr(expr.substring(idx + len));
                bool eq = (l.isStr || r.isStr) ? (l.asStr() == r.asStr()) : (fabs(l.asNum() - r.asNum()) < 0.00001);
                if (strcmp(op, "==") == 0 || strcmp(op, "===") == 0) return JsVal(eq ? 1 : 0);
                if (strcmp(op, "!=") == 0 || strcmp(op, "!==") == 0) return JsVal(!eq ? 1 : 0);
                if (strcmp(op, "<=") == 0) return JsVal(l.asNum() <= r.asNum() ? 1 : 0);
                if (strcmp(op, ">=") == 0) return JsVal(l.asNum() >= r.asNum() ? 1 : 0);
                if (strcmp(op, "<") == 0)  return JsVal(l.asNum() < r.asNum() ? 1 : 0);
                if (strcmp(op, ">") == 0)  return JsVal(l.asNum() > r.asNum() ? 1 : 0);
            }
        }

        // 4. Addition / String-Konkatenation & Subtraktion (von rechts nach links für Linksassoziativität)
        for (int i = (int)expr.length() - 1, paren = 0; i >= 0; i--) {
            char c = expr.charAt(i);
            if (c == '"' || c == '\'') {
                char q = c; i--;
                while (i >= 0 && (expr.charAt(i) != q || (i > 0 && expr.charAt(i - 1) == '\\'))) i--;
                continue;
            }
            if (c == ')' || c == ']' || c == '}') { paren++; continue; }
            if (c == '(' || c == '[' || c == '{') { paren--; continue; }
            if (paren == 0 && (c == '+' || c == '-')) {
                int p = i - 1;
                while (p >= 0 && expr.charAt(p) == ' ') p--;
                if (p < 0) continue;
                char pc = expr.charAt(p);
                if (pc == '+' || pc == '-' || pc == '*' || pc == '/' || pc == '%' || pc == '=' || pc == '<' || pc == '>' || pc == '!' || pc == '&' || pc == '|' || pc == '?' || pc == ':' || pc == ',') continue;
                JsVal l = evalExpr(expr.substring(0, i));
                JsVal r = evalExpr(expr.substring(i + 1));
                if (c == '+') {
                    if (l.isStr || r.isStr) return JsVal(l.asStr() + r.asStr());
                    return JsVal(l.asNum() + r.asNum());
                } else {
                    return JsVal(l.asNum() - r.asNum());
                }
            }
        }

        // 5. Multiplikation / Division / Modulo
        for (int i = (int)expr.length() - 1, paren = 0; i >= 0; i--) {
            char c = expr.charAt(i);
            if (c == '"' || c == '\'') {
                char q = c; i--;
                while (i >= 0 && (expr.charAt(i) != q || (i > 0 && expr.charAt(i - 1) == '\\'))) i--;
                continue;
            }
            if (c == ')' || c == ']' || c == '}') { paren++; continue; }
            if (c == '(' || c == '[' || c == '{') { paren--; continue; }
            if (paren == 0 && (c == '*' || c == '/' || c == '%')) {
                JsVal l = evalExpr(expr.substring(0, i));
                JsVal r = evalExpr(expr.substring(i + 1));
                double rn = r.asNum();
                if (c == '*') return JsVal(l.asNum() * rn);
                if (c == '/') return JsVal(fabs(rn) < 1e-9 ? 0.0 : (l.asNum() / rn));
                if (c == '%') return JsVal((long)rn == 0 ? 0.0 : (double)((long)l.asNum() % (long)rn));
            }
        }

        // 6. Unäre Operatoren (!, -)
        if (expr.charAt(0) == '!') return JsVal(evalExpr(expr.substring(1)).asBool() ? 0 : 1);
        if (expr.charAt(0) == '-') return JsVal(-evalExpr(expr.substring(1)).asNum());

        // 7. String-Literale ('...' oder "...")
        if ((expr.charAt(0) == '\'' && expr.charAt(expr.length() - 1) == '\'') ||
            (expr.charAt(0) == '"' && expr.charAt(expr.length() - 1) == '"')) {
            String raw = expr.substring(1, expr.length() - 1);
            String out = "";
            for (int i = 0; i < (int)raw.length(); i++) {
                if (raw.charAt(i) == '\\' && i + 1 < (int)raw.length()) {
                    char n = raw.charAt(++i);
                    if (n == 'n') out += '\n';
                    else out += n;
                } else out += raw.charAt(i);
            }
            return JsVal(out);
        }

        // 8. Booleans & Zahlen
        if (expr == "true") return JsVal(1);
        if (expr == "false") return JsVal(0);
        if (isDigit(expr.charAt(0)) || (expr.charAt(0) == '.' && expr.length() > 1)) {
            return JsVal(expr.toDouble());
        }

        // 9. Funktionsaufruf: fn(args) oder obj.method(args)
        int openP = expr.indexOf('(');
        if (openP > 0 && findMatchingBracket(expr, openP) == (int)expr.length() - 1) {
            String callee = expr.substring(0, openP);
            callee.trim();
            std::vector<JsVal> args = evalArgs(expr.substring(openP + 1, expr.length() - 1));

            // String-Methoden z.B. state.display.slice(0, -1) oder str.substring(a, b)
            int dot = callee.lastIndexOf('.');
            if (dot > 0 && !callee.startsWith("Watch.") && !callee.startsWith("Math.")) {
                JsVal target = evalExpr(callee.substring(0, dot));
                String method = callee.substring(dot + 1);
                String ts = target.asStr();
                if (method == "slice" || method == "substring") {
                    int s = args.size() >= 1 ? (int)args[0].asNum() : 0;
                    int e = args.size() >= 2 ? (int)args[1].asNum() : (int)ts.length();
                    if (s < 0) s = max(0, (int)ts.length() + s);
                    if (e < 0) e = max(0, (int)ts.length() + e);
                    if (s > (int)ts.length()) s = ts.length();
                    if (e > (int)ts.length()) e = ts.length();
                    return JsVal(s < e ? ts.substring(s, e) : String(""));
                }
                if (method == "indexOf" && !args.empty()) {
                    return JsVal((double)ts.indexOf(args[0].asStr()));
                }
            }
            return callFunction(callee, args);
        }

        // 10. Variablen & Properties (state.xyz, local, .length)
        if (expr.endsWith(".length")) {
            JsVal target = evalExpr(expr.substring(0, expr.length() - 7));
            return JsVal((double)target.asStr().length());
        }
        if (expr.startsWith("state.")) {
            String prop = expr.substring(6);
            prop.trim();
            return state.count(prop) ? state[prop] : JsVal(0);
        }
        if (locals.count(expr)) return locals[expr];
        if (state.count(expr)) return state[expr];

        return JsVal(0);
    }

    void assignVar(String lhs, const JsVal &val) {
        lhs.trim();
        if (lhs.startsWith("var ")) lhs = lhs.substring(4);
        else if (lhs.startsWith("let ")) lhs = lhs.substring(4);
        else if (lhs.startsWith("const ")) lhs = lhs.substring(6);
        lhs.trim();

        if (lhs.startsWith("state.")) {
            String prop = lhs.substring(6);
            prop.trim();
            state[prop] = val;
        } else {
            locals[lhs] = val;
        }
    }

    void execStatement(String stmt) {
        stmt.trim();
        if (stmt.length() == 0 || hasReturned) return;

        if (stmt == "return" || stmt.startsWith("return ")) {
            hasReturned = true;
            returnVal = (stmt.length() > 6) ? evalExpr(stmt.substring(6)) : JsVal(0);
            return;
        }

        // Postfix ++ / --
        if (stmt.endsWith("++") || stmt.endsWith("--")) {
            bool isInc = stmt.endsWith("++");
            String vName = stmt.substring(0, stmt.length() - 2);
            vName.trim();
            double cur = evalExpr(vName).asNum();
            assignVar(vName, JsVal(isInc ? (cur + 1.0) : (cur - 1.0)));
            return;
        }

        // Compound Assignments (+=, -=, *=, /=)
        const char* compOps[] = { "+=", "-=", "*=", "/=" };
        for (const char* cop : compOps) {
            int idx = findTopLevel(stmt, cop);
            if (idx != -1) {
                String lhs = stmt.substring(0, idx);
                JsVal cur = evalExpr(lhs);
                JsVal rhs = evalExpr(stmt.substring(idx + 2));
                if (strcmp(cop, "+=") == 0) {
                    assignVar(lhs, (cur.isStr || rhs.isStr) ? JsVal(cur.asStr() + rhs.asStr()) : JsVal(cur.asNum() + rhs.asNum()));
                } else if (strcmp(cop, "-=") == 0) {
                    assignVar(lhs, JsVal(cur.asNum() - rhs.asNum()));
                } else if (strcmp(cop, "*=") == 0) {
                    assignVar(lhs, JsVal(cur.asNum() * rhs.asNum()));
                } else if (strcmp(cop, "/=") == 0) {
                    double r = rhs.asNum();
                    assignVar(lhs, JsVal(fabs(r) < 1e-9 ? 0.0 : cur.asNum() / r));
                }
                return;
            }
        }

        // Standard Assignment (=)
        int eqIdx = findTopLevel(stmt, "=");
        if (eqIdx != -1) {
            String lhs = stmt.substring(0, eqIdx);
            String rhs = stmt.substring(eqIdx + 1);
            assignVar(lhs, evalExpr(rhs));
            return;
        }

        // Reiner Ausdruck / Funktionsaufruf
        evalExpr(stmt);
    }

    void execBlock(const String &block) {
        int i = 0;
        int n = block.length();
        while (i < n && !hasReturned) {
            while (i < n && (block.charAt(i) == ' ' || block.charAt(i) == '\t' || block.charAt(i) == '\r' || block.charAt(i) == '\n' || block.charAt(i) == ';')) i++;
            if (i >= n) break;

            // Funktionsdeklarationen auf Blockebene überspringen
            if (block.substring(i).startsWith("function ")) {
                int pOpen = block.indexOf('(', i);
                int pClose = (pOpen != -1) ? findMatchingBracket(block, pOpen) : -1;
                int bOpen = (pClose != -1) ? block.indexOf('{', pClose) : -1;
                int bClose = (bOpen != -1) ? findMatchingBracket(block, bOpen) : -1;
                if (bClose != -1) { i = bClose + 1; continue; }
                break;
            }

            // IF / ELSE IF / ELSE
            if (block.substring(i).startsWith("if") && (i + 2 < n) && (block.charAt(i + 2) == ' ' || block.charAt(i + 2) == '(')) {
                bool branchTaken = false;
                while (i < n) {
                    while (i < n && (block.charAt(i) == ' ' || block.charAt(i) == '\t' || block.charAt(i) == '\r' || block.charAt(i) == '\n')) i++;
                    bool isIf = false;
                    if (block.substring(i).startsWith("if") && (block.charAt(i + 2) == ' ' || block.charAt(i + 2) == '(')) {
                        isIf = true;
                        i += 2;
                    } else if (block.substring(i).startsWith("else")) {
                        i += 4;
                        while (i < n && (block.charAt(i) == ' ' || block.charAt(i) == '\t' || block.charAt(i) == '\r' || block.charAt(i) == '\n')) i++;
                        if (block.substring(i).startsWith("if") && (block.charAt(i + 2) == ' ' || block.charAt(i + 2) == '(')) {
                            isIf = true;
                            i += 2;
                        }
                    } else {
                        break;
                    }

                    bool condVal = true;
                    if (isIf) {
                        int cOpen = block.indexOf('(', i);
                        int cClose = (cOpen != -1) ? findMatchingBracket(block, cOpen) : -1;
                        if (cClose == -1) return;
                        if (!branchTaken) {
                            condVal = evalExpr(block.substring(cOpen + 1, cClose)).asBool();
                        }
                        i = cClose + 1;
                    }

                    while (i < n && (block.charAt(i) == ' ' || block.charAt(i) == '\t' || block.charAt(i) == '\r' || block.charAt(i) == '\n')) i++;
                    if (i < n && block.charAt(i) == '{') {
                        int bClose = findMatchingBracket(block, i);
                        if (bClose == -1) return;
                        if (!branchTaken && condVal) {
                            branchTaken = true;
                            execBlock(block.substring(i + 1, bClose));
                        }
                        i = bClose + 1;
                    } else {
                        int sEnd = block.indexOf(';', i);
                        if (sEnd == -1) sEnd = n;
                        if (!branchTaken && condVal) {
                            branchTaken = true;
                            execStatement(block.substring(i, sEnd));
                        }
                        i = sEnd + 1;
                    }

                    int peek = i;
                    while (peek < n && (block.charAt(peek) == ' ' || block.charAt(peek) == '\t' || block.charAt(peek) == '\r' || block.charAt(peek) == '\n')) peek++;
                    if (!block.substring(peek).startsWith("else")) break;
                    i = peek;
                }
                continue;
            }

            // WHILE Loop (mit Iterations-Schutz)
            if (block.substring(i).startsWith("while") && (i + 5 < n) && (block.charAt(i + 5) == ' ' || block.charAt(i + 5) == '(')) {
                int cOpen = block.indexOf('(', i + 5);
                int cClose = (cOpen != -1) ? findMatchingBracket(block, cOpen) : -1;
                if (cClose == -1) break;
                int bOpen = block.indexOf('{', cClose + 1);
                int bClose = (bOpen != -1) ? findMatchingBracket(block, bOpen) : -1;
                if (bClose == -1) break;
                String condExpr = block.substring(cOpen + 1, cClose);
                String bodyStr = block.substring(bOpen + 1, bClose);
                int guard = 0;
                while (!hasReturned && guard++ < 250 && evalExpr(condExpr).asBool()) {
                    execBlock(bodyStr);
                }
                i = bClose + 1;
                continue;
            }

            // Einzelnes Statement bis ';' oder Zeilenende suchen
            int sStart = i;
            int paren = 0;
            bool inStr = false;
            char q = 0;
            while (i < n) {
                char c = block.charAt(i);
                if (inStr) {
                    if (c == '\\') { i += 2; continue; }
                    if (c == q) inStr = false;
                    i++;
                    continue;
                }
                if (c == '"' || c == '\'') { inStr = true; q = c; i++; continue; }
                if (c == '(' || c == '[' || c == '{') paren++;
                else if (c == ')' || c == ']' || c == '}') paren--;
                else if (paren == 0 && (c == ';' || c == '\n')) {
                    break;
                }
                i++;
            }
            execStatement(block.substring(sStart, i));
            if (i < n) i++;
        }
    }
};

static void ensureJsAppInitialized(AppPackage *app) {
    if (!app || app->jsInitialized) return;
    app->jsInitialized = true;
    parseInitialStateFromLogic(app->id, app->logicJson);
    if (app->jsScript.length() > 0 && app->jsScript.indexOf("function onInit") != -1) {
        JsVM vm(app, app->jsScript);
        vm.callFunction("onInit", {});
        syncSystemGlobalsFromState(app);
    }
}

static void runJsAction(AppPackage *app, const String &actionExpr) {
    if (!app || app->jsScript.length() == 0) return;
    ensureJsAppInitialized(app);
    JsVM vm(app, app->jsScript);
    if (actionExpr.indexOf('(') != -1) {
        vm.execBlock(actionExpr);
    } else {
        vm.callFunction(actionExpr, {});
    }
    syncSystemGlobalsFromState(app);
}

static String interpolateStateVars(const String &appId, const String &tmpl) {
    if (tmpl.indexOf('{') == -1) return tmpl;
    auto &st = appStates[appId];
    String out = "";
    int i = 0;
    while (i < (int)tmpl.length()) {
        int b1 = tmpl.indexOf('{', i);
        if (b1 == -1) {
            out += tmpl.substring(i);
            break;
        }
        out += tmpl.substring(i, b1);
        int b2 = tmpl.indexOf('}', b1 + 1);
        if (b2 == -1) {
            out += tmpl.substring(b1);
            break;
        }
        String varName = tmpl.substring(b1 + 1, b2);
        varName.trim();
        if (st.count(varName)) {
            out += st[varName].asStr();
        }
        i = b2 + 1;
    }
    return out;
}

bool AppManager::hasJsScript(const AppPackage *app) {
    return app && app->jsScript.length() > 0 && app->uiJson.length() > 0;
}

void AppManager::tickDynamicApp(AppPackage *app, unsigned long now) {
    if (!hasJsScript(app)) return;
    ensureJsAppInitialized(app);
    int interval = app->tickIntervalMs > 0 ? app->tickIntervalMs : 100;
    if (now - app->lastTickMs < (unsigned long)interval) return;
    app->lastTickMs = now;

    if (app->jsScript.indexOf("function onTick") != -1) {
        String prevTime = appStates[app->id].count("timeStr") ? appStates[app->id]["timeStr"].asStr() : "";
        JsVM vm(app, app->jsScript);
        vm.callFunction("onTick", {});
        syncSystemGlobalsFromState(app);
        String newTime = appStates[app->id].count("timeStr") ? appStates[app->id]["timeStr"].asStr() : "";
        if (prevTime != newTime || appStates[app->id]["running"].asBool()) {
            lastSecond = -1;
        }
    }
}

void AppManager::tickBackgroundApps(unsigned long now) {
    for (auto &pkg : registry) {
        if (!hasJsScript(&pkg)) continue;
        if (!pkg.jsInitialized) continue;
        bool isRun = appStates[pkg.id].count("running") ? appStates[pkg.id]["running"].asBool() : false;
        if (isRun) {
            tickDynamicApp(&pkg, now);
        }
    }
}

void AppManager::stopAllJsApps() {
    for (auto &pkg : registry) {
        if (appStates.count(pkg.id) && appStates[pkg.id].count("running")) {
            if (appStates[pkg.id]["running"].asBool()) {
                if (pkg.jsScript.indexOf("function reset") != -1) {
                    runJsAction(&pkg, "reset()");
                } else if (pkg.jsScript.indexOf("function cancel") != -1) {
                    runJsAction(&pkg, "cancel()");
                } else {
                    appStates[pkg.id]["running"] = JsVal(0);
                    syncSystemGlobalsFromState(&pkg);
                }
            }
        }
    }
}

void AppManager::reassignDynamicPages() {
    for (size_t i = 0; i < registry.size(); i++) {
        registry[i].pageId = DYNAMIC_PAGE_BASE + (int)i;
    }
}

// ============================================================================
// Standard .app Pakete (inkl. echtem JavaScript in "logic.script")
// ============================================================================
String AppManager::getDefaultAppPackageJson(const String &id) {
    if (id == "stopwatch") {
        return R"rawliteral({
  "id": "stopwatch",
  "name": "Stoppuhr",
  "category": "productivity",
  "color": "#ffea00",
  "icon": "SW",
  "version": "1.1.0",
  "author": "WatchOS",
  "description": "Präzisions-Stoppuhr mit Rundenzeiten und 7-Segment-Anzeige (JS)",
  "driver": "stopwatch",
  "ui": {
    "title": "Stoppuhr",
    "elements": [
      {
        "type": "7segment",
        "id": "display",
        "text": "{timeStr}",
        "y": 25,
        "h": 22,
        "w": 12,
        "spacing": 4,
        "color": "#ffea00"
      },
      {
        "type": "label",
        "id": "lap1",
        "text": "{lap1}",
        "x": 120,
        "y": 64,
        "size": 2,
        "color": "#777777"
      },
      {
        "type": "label",
        "id": "lap2",
        "text": "{lap2}",
        "x": 120,
        "y": 82,
        "size": 2,
        "color": "#999999"
      },
      {
        "type": "label",
        "id": "lap3",
        "text": "{lap3}",
        "x": 120,
        "y": 100,
        "size": 2,
        "color": "#00ff00"
      },
      {
        "type": "button",
        "id": "start_stop",
        "text": "{btnText}",
        "x": 5,
        "y": 150,
        "w": 72,
        "h": 40,
        "color": "{btnColor}",
        "text_color": "#000000",
        "action": "toggle()"
      },
      {
        "type": "button",
        "id": "lap",
        "text": "Runde",
        "x": 84,
        "y": 150,
        "w": 72,
        "h": 40,
        "color": "{lapColor}",
        "text_color": "#000000",
        "action": "lap()"
      },
      {
        "type": "button",
        "id": "reset",
        "text": "Reset",
        "x": 163,
        "y": 150,
        "w": 72,
        "h": 40,
        "color": "#333333",
        "text_color": "#ffffff",
        "action": "reset()"
      }
    ]
  },
  "logic": {
    "lang": "javascript",
    "tick_ms": 40,
    "state": {
      "running": 0,
      "startTime": 0,
      "elapsed": 0,
      "lapCount": 0,
      "timeStr": "00:00.00",
      "btnText": "Start",
      "btnColor": "#00ff00",
      "lapColor": "#333333",
      "lap1": "",
      "lap2": "",
      "lap3": ""
    },
    "script": "function onTick() {\n  if (state.running == 1) {\n    var ms = state.elapsed + (Watch.millis() - state.startTime);\n    state.timeStr = Watch.formatTimeMs(ms);\n  }\n}\n\nfunction toggle() {\n  Watch.vibrate();\n  if (state.running == 0) {\n    state.startTime = Watch.millis();\n    state.running = 1;\n    state.btnText = 'Stop';\n    state.btnColor = '#ff9900';\n    state.lapColor = '#00ff00';\n  } else {\n    state.elapsed = state.elapsed + (Watch.millis() - state.startTime);\n    state.running = 0;\n    state.btnText = 'Start';\n    state.btnColor = '#00ff00';\n    state.lapColor = '#333333';\n    state.timeStr = Watch.formatTimeMs(state.elapsed);\n  }\n}\n\nfunction lap() {\n  if (state.running == 1) {\n    Watch.vibrate();\n    state.lapCount = state.lapCount + 1;\n    var ms = state.elapsed + (Watch.millis() - state.startTime);\n    var entry = 'R' + state.lapCount + '  ' + Watch.formatTimeMs(ms);\n    state.lap1 = state.lap2;\n    state.lap2 = state.lap3;\n    state.lap3 = entry;\n  }\n}\n\nfunction reset() {\n  Watch.vibrate();\n  state.running = 0;\n  state.elapsed = 0;\n  state.lapCount = 0;\n  state.timeStr = '00:00.00';\n  state.btnText = 'Start';\n  state.btnColor = '#00ff00';\n  state.lapColor = '#333333';\n  state.lap1 = '';\n  state.lap2 = '';\n  state.lap3 = '';\n}"
  }
})rawliteral";
    }
    if (id == "timer") {
        return R"rawliteral({
  "id": "timer",
  "name": "Timer",
  "category": "productivity",
  "color": "#00ff00",
  "icon": "TI",
  "version": "1.1.0",
  "author": "WatchOS",
  "description": "Countdown-Timer mit Schnellwahltasten und Alarmton (JS)",
  "driver": "timer",
  "ui": {
    "title": "Timer",
    "elements": [
      {
        "type": "label",
        "id": "lbl_choose",
        "text": "Waehlen:",
        "x": 120,
        "y": 50,
        "size": 4,
        "color": "#00ff00",
        "visible": "{isIdle}"
      },
      {
        "type": "button",
        "id": "p1",
        "text": "3m",
        "x": 20,
        "y": 80,
        "w": 60,
        "h": 50,
        "color": "#333333",
        "text_color": "#ffffff",
        "visible": "{isIdle}",
        "action": "start(3)"
      },
      {
        "type": "button",
        "id": "p2",
        "text": "5m",
        "x": 90,
        "y": 80,
        "w": 60,
        "h": 50,
        "color": "#333333",
        "text_color": "#ffffff",
        "visible": "{isIdle}",
        "action": "start(5)"
      },
      {
        "type": "button",
        "id": "p3",
        "text": "10m",
        "x": 160,
        "y": 80,
        "w": 60,
        "h": 50,
        "color": "#333333",
        "text_color": "#ffffff",
        "visible": "{isIdle}",
        "action": "start(10)"
      },
      {
        "type": "7segment",
        "id": "display",
        "text": "{timeStr}",
        "y": 65,
        "h": 38,
        "w": 22,
        "spacing": 6,
        "color": "#ff0000",
        "visible": "{running}"
      },
      {
        "type": "button",
        "id": "cancel",
        "text": "Abbruch",
        "x": 75,
        "y": 150,
        "w": 90,
        "h": 40,
        "color": "#ff0000",
        "text_color": "#ffffff",
        "visible": "{running}",
        "action": "cancel()"
      }
    ]
  },
  "logic": {
    "lang": "javascript",
    "tick_ms": 200,
    "state": {
      "running": 0,
      "isIdle": 1,
      "endTime": 0,
      "remainingSec": 0,
      "timeStr": "00:00"
    },
    "script": "function start(mins) {\n  Watch.vibrate();\n  state.remainingSec = mins * 60;\n  state.endTime = Watch.millis() + (mins * 60000);\n  state.running = 1;\n  state.isIdle = 0;\n  state.timeStr = Watch.formatTimeSec(state.remainingSec);\n}\n\nfunction onTick() {\n  if (state.running == 1) {\n    var leftMs = state.endTime - Watch.millis();\n    if (leftMs <= 0) {\n      state.running = 0;\n      state.isIdle = 1;\n      state.remainingSec = 0;\n      state.timeStr = '00:00';\n      Watch.alarm();\n      Watch.toast('Timer abgelaufen!');\n    } else {\n      state.remainingSec = Math.ceil(leftMs / 1000);\n      state.timeStr = Watch.formatTimeSec(state.remainingSec);\n    }\n  }\n}\n\nfunction cancel() {\n  Watch.vibrate();\n  state.running = 0;\n  state.isIdle = 1;\n  state.remainingSec = 0;\n  state.timeStr = '00:00';\n}"
  }
})rawliteral";
    }
    if (id == "mic") {
        return R"rawliteral({
  "id": "mic",
  "name": "Dezibel",
  "category": "productivity",
  "color": "#00ff00",
  "icon": "DB",
  "version": "1.0.0",
  "author": "WatchOS",
  "description": "Schallpegelmesser mit I2S-Mikrofon und RMS-Balkengrafik",
  "driver": "mic",
  "ui": {
    "title": "Dezibel-Meter",
    "readout": {
      "type": "db_spl",
      "y": 50,
      "size": 6,
      "color": "#ffffff"
    },
    "rms_label": {
      "type": "label",
      "format": "RMS: {rms}",
      "y": 85,
      "color": "#888888"
    },
    "meter": {
      "type": "bar_graph",
      "x": 60,
      "y": 74,
      "w": 120,
      "h": 120
    }
  },
  "logic": {
    "engine": "i2s_microphone",
    "sample_rate": 16000,
    "unit": "dB SPL",
    "auto_power_save": true
  }
})rawliteral";
    }
    if (id == "calc") {
        return R"rawliteral({
  "id": "calc",
  "name": "Rechner",
  "category": "productivity",
  "color": "#ffffff",
  "icon": "CA",
  "version": "1.1.0",
  "author": "WatchOS",
  "description": "Taschenrechner mit Touch-Bedienung und JS-Rechenlogik",
  "driver": "calc",
  "ui": {
    "title": "",
    "elements": [
      {
        "type": "lcd",
        "id": "display",
        "text": "{display}",
        "x": 10,
        "y": 4,
        "w": 220,
        "h": 36,
        "bg": "#111822",
        "color": "#00ff88",
        "size": 4
      },
      {
        "type": "keypad",
        "id": "pad",
        "x": 10,
        "y": 44,
        "w": 220,
        "h": 156,
        "rows": 4,
        "cols": 4,
        "keys": "7,8,9,/,4,5,6,*,1,2,3,-,C,0,=,+"
      }
    ]
  },
  "logic": {
    "lang": "javascript",
    "state": {
      "display": "0",
      "justEvaluated": 0
    },
    "script": "function onKey(k) {\n  Watch.vibrate();\n  if (k == 'C') {\n    state.display = '0';\n    state.justEvaluated = 0;\n    return;\n  }\n  if (k == '=') {\n    var res = Watch.evalCalc(state.display);\n    state.display = String(res);\n    state.justEvaluated = 1;\n    return;\n  }\n  var isOp = (k == '+' || k == '-' || k == '*' || k == '/');\n  if (state.display == '0' || state.display == 'Error' || (state.justEvaluated == 1 && !isOp)) {\n    if (isOp) {\n      state.justEvaluated = 0;\n      state.display = state.display + k;\n    } else {\n      state.display = k;\n      state.justEvaluated = 0;\n    }\n  } else {\n    state.justEvaluated = 0;\n    if (state.display.length < 14) {\n      state.display = state.display + k;\n    }\n  }\n}"
  }
})rawliteral";
    }
    if (id == "jiggler") {
        return R"rawliteral({
  "id": "jiggler",
  "name": "Jiggler",
  "category": "productivity",
  "color": "#00ffff",
  "icon": "MJ",
  "version": "1.1.0",
  "author": "WatchOS",
  "description": "USB HID Mouse Mover mit JS-Bewegungslogik",
  "driver": "jiggler",
  "ui": {
    "title": "Mouse Jiggler",
    "elements": [
      {
        "type": "card",
        "id": "status_card",
        "text": "{statusText}",
        "x": 20,
        "y": 35,
        "w": 200,
        "h": 50,
        "color": "{statusColor}",
        "size": 4
      },
      {
        "type": "label",
        "id": "moves_lbl",
        "text": "{infoText}",
        "x": 120,
        "y": 105,
        "size": 2,
        "color": "#888888"
      },
      {
        "type": "button",
        "id": "btn_toggle",
        "text": "{btnText}",
        "x": 40,
        "y": 135,
        "w": 160,
        "h": 45,
        "color": "{btnColor}",
        "text_color": "#000000",
        "action": "toggle()"
      }
    ]
  },
  "logic": {
    "lang": "javascript",
    "tick_ms": 1000,
    "state": {
      "running": 0,
      "step": 0,
      "moves": 0,
      "statusText": "PAUSIERT",
      "statusColor": "#555555",
      "infoText": "USB HID Maus-Bewegung",
      "btnText": "Jiggler Starten",
      "btnColor": "#00ffff"
    },
    "script": "function toggle() {\n  Watch.vibrate();\n  if (state.running == 0) {\n    state.running = 1;\n    state.statusText = 'AKTIV';\n    state.statusColor = '#00ff00';\n    state.btnText = 'Jiggler Stoppen';\n    state.btnColor = '#ff9900';\n  } else {\n    state.running = 0;\n    state.statusText = 'PAUSIERT';\n    state.statusColor = '#555555';\n    state.btnText = 'Jiggler Starten';\n    state.btnColor = '#00ffff';\n  }\n}\n\nfunction onTick() {\n  if (state.running == 1) {\n    var dx = (state.step % 2 == 0) ? 4 : -4;\n    Watch.jiggle(dx, 0);\n    state.step = state.step + 1;\n    state.moves = state.moves + 1;\n    state.infoText = 'Impulse: ' + state.moves;\n  }\n}"
  }
})rawliteral";
    }
    if (id == "dice") {
        return R"rawliteral({
  "id": "dice",
  "name": "Wuerfel",
  "category": "productivity",
  "color": "#ffff00",
  "icon": "D6",
  "version": "1.1.0",
  "author": "WatchOS",
  "description": "Würfel (d4-d100) und Münzwurf mit JS-Zufallslogik",
  "driver": "dice",
  "ui": {
    "title": "Wuerfel & Muenze",
    "elements": [
      {
        "type": "button",
        "id": "m_d6",
        "text": "D6",
        "x": 10,
        "y": 24,
        "w": 50,
        "h": 28,
        "color": "#333333",
        "text_color": "#ffff00",
        "action": "setSides(6)"
      },
      {
        "type": "button",
        "id": "m_d20",
        "text": "D20",
        "x": 66,
        "y": 24,
        "w": 50,
        "h": 28,
        "color": "#333333",
        "text_color": "#00e5ff",
        "action": "setSides(20)"
      },
      {
        "type": "button",
        "id": "m_d100",
        "text": "D100",
        "x": 122,
        "y": 24,
        "w": 52,
        "h": 28,
        "color": "#333333",
        "text_color": "#00ff88",
        "action": "setSides(100)"
      },
      {
        "type": "button",
        "id": "m_coin",
        "text": "Münze",
        "x": 180,
        "y": 24,
        "w": 50,
        "h": 28,
        "color": "#333333",
        "text_color": "#ff9900",
        "action": "setSides(2)"
      },
      {
        "type": "card",
        "id": "face",
        "text": "{result}",
        "x": 35,
        "y": 60,
        "w": 170,
        "h": 76,
        "color": "#ffff00",
        "size": 6
      },
      {
        "type": "button",
        "id": "btn_roll",
        "text": "{modeLabel} werfen",
        "x": 25,
        "y": 148,
        "w": 190,
        "h": 44,
        "color": "#ffff00",
        "text_color": "#000000",
        "action": "roll()"
      }
    ]
  },
  "logic": {
    "lang": "javascript",
    "state": {
      "sides": 6,
      "result": "6",
      "modeLabel": "D6"
    },
    "script": "function setSides(n) {\n  Watch.vibrate();\n  state.sides = n;\n  if (n == 2) {\n    state.modeLabel = 'Muenze';\n    state.result = 'KOPF';\n  } else {\n    state.modeLabel = 'D' + n;\n    state.result = String(n);\n  }\n}\n\nfunction roll() {\n  Watch.vibrate();\n  if (state.sides == 2) {\n    var c = Watch.random(0, 2);\n    state.result = (c == 0) ? 'KOPF' : 'ZAHL';\n  } else {\n    var r = Watch.random(1, state.sides + 1);\n    state.result = String(r);\n  }\n}"
  }
})rawliteral";
    }
    if (id == "level") {
        return R"rawliteral({
  "id": "level",
  "name": "Waage",
  "category": "productivity",
  "color": "#ffb600",
  "icon": "LV",
  "version": "1.0.0",
  "author": "WatchOS",
  "description": "2D-Wasserwaage per BMA423 Gyro/Beschleunigung",
  "driver": "level",
  "ui": {
    "title": "Wasserwaage",
    "crosshair": {
      "type": "circle_target",
      "cx": 120,
      "cy": 110,
      "r": 70,
      "color": "#333333"
    },
    "bubble": {
      "type": "bubble",
      "r": 14,
      "color": "#00ff00"
    },
    "angles": {
      "type": "readout",
      "format": "X: {roll}°  Y: {pitch}°",
      "y": 195,
      "color": "#ffffff"
    }
  },
  "logic": {
    "engine": "bma423_level",
    "axes": [
      "roll",
      "pitch"
    ],
    "damping": 0.8
  }
})rawliteral";
    }
    if (id == "totp") {
        return R"rawliteral({
  "id": "totp",
  "name": "2FA TOTP",
  "category": "productivity",
  "color": "#00ffff",
  "icon": "2F",
  "version": "1.1.0",
  "author": "WatchOS",
  "description": "Zwei-Faktor-Authenticator nach RFC 6238",
  "driver": "totp",
  "ui": {
    "title": "2FA Authenticator",
    "service": {
      "type": "label",
      "source": "service_name",
      "y": 50,
      "size": 2,
      "color": "#ffffff"
    },
    "code": {
      "type": "totp_display",
      "format": "000 000",
      "y": 85,
      "size": 6,
      "color": "#00ffff"
    },
    "progress": {
      "type": "countdown_ring",
      "period": 30,
      "y": 150,
      "color": "#00e5ff"
    }
  },
  "logic": {
    "engine": "rfc6238_authenticator",
    "period_sec": 30,
    "digits": 6,
    "algorithm": "SHA1"
  }
})rawliteral";
    }
    if (id == "lora") {
        return R"rawliteral({
  "id": "lora",
  "name": "LoRa Funk",
  "category": "productivity",
  "color": "#00ffff",
  "icon": "RF",
  "version": "2.0.0",
  "author": "WatchOS",
  "description": "Off-Grid P2P Messenger & Beacon auf 868MHz",
  "driver": "lora",
  "ui": {
    "title": "LoRa Funk",
    "tabs": [
      "Chat",
      "Beacon"
    ],
    "presets": [
      {
        "text": "Hallo!",
        "x": 10,
        "y": 160,
        "w": 52,
        "h": 36
      },
      {
        "text": "OK",
        "x": 66,
        "y": 160,
        "w": 52,
        "h": 36
      },
      {
        "text": "Ping?",
        "x": 122,
        "y": 160,
        "w": 52,
        "h": 36
      },
      {
        "text": "Hilfe!",
        "x": 178,
        "y": 160,
        "w": 52,
        "h": 36
      }
    ],
    "beacon_button": {
      "text": "Beacon An/Aus",
      "y": 150,
      "color": "#00ffff"
    }
  },
  "logic": {
    "engine": "sx1262_lora",
    "frequency_mhz": 868.0,
    "bandwidth_khz": 125.0,
    "spreading_factor": 7,
    "tx_power_dbm": 22
  }
})rawliteral";
    }
    if (id == "notif") {
        return R"rawliteral({
  "id": "notif",
  "name": "Mitteilung",
  "category": "productivity",
  "color": "#9cbeff",
  "icon": "NT",
  "version": "1.0.0",
  "author": "WatchOS",
  "description": "Push-Mitteilungszentrale vom Smartphone",
  "driver": "notif",
  "ui": {
    "title": "Mitteilungen",
    "list": {
      "type": "notification_cards",
      "y": 35,
      "max": 4,
      "empty_text": "Keine Mitteilungen"
    },
    "button_clear": {
      "text": "Alle gelesen",
      "y": 190,
      "color": "#9cbeff"
    }
  },
  "logic": {
    "engine": "notification_center",
    "buffer_size": 10,
    "ble_sync": true
  }
})rawliteral";
    }
    if (id == "battery") {
        return R"rawliteral({
  "id": "battery",
  "name": "Akku-Info",
  "category": "productivity",
  "color": "#31ff00",
  "icon": "BA",
  "version": "1.0.0",
  "author": "WatchOS",
  "description": "Detaillierte Telemetrie und Ladehistorie",
  "driver": "battery",
  "ui": {
    "title": "Akku Telemetrie",
    "telemetry_card": {
      "type": "glass_card",
      "x": 8,
      "y": 4,
      "w": 224,
      "h": 64,
      "metrics": [
        "pct",
        "volt",
        "charging",
        "runtime"
      ]
    },
    "graph": {
      "type": "discharge_curve",
      "x": 8,
      "y": 76,
      "w": 224,
      "h": 115,
      "samples": 200
    }
  },
  "logic": {
    "engine": "axp2101_pmu",
    "voltage_adc": true,
    "history_interval_sec": 600
  }
})rawliteral";
    }
    if (id == "flashlight") {
        return R"rawliteral({
  "id": "flashlight",
  "name": "Lampe",
  "category": "productivity",
  "color": "#ffffff",
  "icon": "FL",
  "version": "1.0.0",
  "author": "WatchOS",
  "description": "Helles Display-Flutlicht mit maximaler Helligkeit",
  "driver": "flashlight",
  "ui": {
    "title": "Taschenlampe",
    "display": {
      "type": "fullscreen",
      "color": "#ffffff"
    }
  },
  "logic": {
    "engine": "backlight_boost",
    "brightness": 255,
    "lock_awake": true,
    "dismiss_on_tap": true
  }
})rawliteral";
    }
    if (id == "phishing") {
        return R"rawliteral({
  "id": "phishing",
  "name": "Phishing",
  "category": "security",
  "color": "#ff0000",
  "icon": "EV",
  "version": "1.0.0",
  "author": "WatchOS",
  "description": "Captive Portal zur Demonstration von Phishing",
  "driver": "phishing",
  "ui": {
    "title": "Evil Portal",
    "status": {
      "type": "badge",
      "y": 40,
      "active": "PORTAL AKTIV",
      "idle": "INAKTIV"
    },
    "creds": {
      "type": "credentials_log",
      "y": 70,
      "max": 3
    },
    "button_toggle": {
      "text": "Portal An/Aus",
      "x": 40,
      "y": 160,
      "w": 160,
      "h": 40,
      "color": "#ff0000"
    }
  },
  "logic": {
    "engine": "dns_captive_portal",
    "softap_ssid": "Free_WiFi_Login",
    "dns_hijack": true
  }
})rawliteral";
    }
    if (id == "badusb") {
        return R"rawliteral({
  "id": "badusb",
  "name": "Ducky",
  "category": "security",
  "color": "#ffff00",
  "icon": "DU",
  "version": "1.2.0",
  "author": "WatchOS",
  "description": "Rubber Ducky USB HID Tastatur-Emulation",
  "driver": "badusb",
  "ui": {
    "title": "Rubber Ducky",
    "script_picker": {
      "type": "carousel",
      "y": 45,
      "scripts": [
        "Fake Update",
        "System Info",
        "WiFi Passwords"
      ]
    },
    "button_inject": {
      "text": "START INJEKTION",
      "x": 50,
      "y": 95,
      "w": 140,
      "h": 45,
      "color": "#ff0000"
    }
  },
  "logic": {
    "engine": "usb_hid_keyboard",
    "layout": "DE_ISO"
  }
})rawliteral";
    }
    if (id == "wifispam") {
        return R"rawliteral({
  "id": "wifispam",
  "name": "WiFi Spam",
  "category": "security",
  "color": "#ff0000",
  "icon": "WS",
  "version": "1.0.0",
  "author": "WatchOS",
  "description": "Aussenden von hunderten gefälschten WiFi-Beacons",
  "driver": "wifispam",
  "ui": {
    "title": "WiFi Spam",
    "radar": {
      "type": "beacon_animation",
      "y": 50
    },
    "button_toggle": {
      "text": "SPAM AN/AUS",
      "x": 55,
      "y": 85,
      "w": 130,
      "h": 45,
      "color": "#ff0000"
    }
  },
  "logic": {
    "engine": "80211_beacon_flooder",
    "packet_type": "raw_mgmt_beacon"
  }
})rawliteral";
    }
    if (id == "blespam") {
        return R"rawliteral({
  "id": "blespam",
  "name": "BLE Spam",
  "category": "security",
  "color": "#ff00ff",
  "icon": "BS",
  "version": "1.0.0",
  "author": "WatchOS",
  "description": "BLE Advertisement Flooding & Notification Popups",
  "driver": "blespam",
  "ui": {
    "title": "BLE Spam",
    "ping": {
      "type": "pulse_animation",
      "y": 50
    },
    "button_toggle": {
      "text": "BLE SPAM START",
      "x": 55,
      "y": 85,
      "w": 130,
      "h": 45,
      "color": "#ff00ff"
    }
  },
  "logic": {
    "engine": "ble_adv_flooder"
  }
})rawliteral";
    }
    if (id == "nmap") {
        return R"rawliteral({
  "id": "nmap",
  "name": "Nmap",
  "category": "security",
  "color": "#00ff00",
  "icon": "NM",
  "version": "1.0.0",
  "author": "WatchOS",
  "description": "Multi-Port Netzwerk-Portscanner",
  "driver": "nmap",
  "ui": {
    "title": "Portscanner",
    "network_picker": {
      "type": "carousel",
      "y": 45
    },
    "button_scan": {
      "text": "SCAN STARTEN",
      "x": 50,
      "y": 95,
      "w": 140,
      "h": 45,
      "color": "#00ff00"
    },
    "results": {
      "type": "port_list",
      "y": 145
    }
  },
  "logic": {
    "engine": "tcp_port_scanner"
  }
})rawliteral";
    }
    if (id == "deauth") {
        return R"rawliteral({
  "id": "deauth",
  "name": "Deauth",
  "category": "security",
  "color": "#ff0000",
  "icon": "DA",
  "version": "1.0.0",
  "author": "WatchOS",
  "description": "802.11 Deauthentication Frame Generator",
  "driver": "deauth",
  "ui": {
    "title": "Deauther",
    "target_picker": {
      "type": "carousel",
      "y": 45
    },
    "button_attack": {
      "text": "DEAUTH ATTACKE",
      "x": 50,
      "y": 85,
      "w": 140,
      "h": 35,
      "color": "#ff0000"
    },
    "button_scan": {
      "text": "Netze Scannen",
      "x": 60,
      "y": 125,
      "w": 120,
      "h": 30
    }
  },
  "logic": {
    "engine": "80211_deauther"
  }
})rawliteral";
    }
    if (id == "probemon") {
        return R"rawliteral({
  "id": "probemon",
  "name": "Probe Mon",
  "category": "security",
  "color": "#00ffff",
  "icon": "PM",
  "version": "1.0.0",
  "author": "WatchOS",
  "description": "Erfassen von Probe Requests mobiler Geräte",
  "driver": "probemon",
  "ui": {
    "title": "Probe Monitor",
    "log": {
      "type": "packet_list",
      "y": 15,
      "lines": 4
    },
    "button_toggle": {
      "text": "Sniffer Start/Stop",
      "x": 40,
      "y": 145,
      "w": 160,
      "h": 40,
      "color": "#00ffff"
    }
  },
  "logic": {
    "engine": "80211_probe_sniffer"
  }
})rawliteral";
    }
    if (id == "blescan") {
        return R"rawliteral({
  "id": "blescan",
  "name": "BLE Scan",
  "category": "security",
  "color": "#ff00ff",
  "icon": "BC",
  "version": "1.0.0",
  "author": "WatchOS",
  "description": "BLE Umgebungsscanner & Device Discovery",
  "driver": "blescan",
  "ui": {
    "title": "BLE Scanner",
    "device_list": {
      "type": "rssi_list",
      "y": 15,
      "lines": 4
    },
    "button_toggle": {
      "text": "Scan Start/Stop",
      "x": 40,
      "y": 145,
      "w": 160,
      "h": 40,
      "color": "#ff00ff"
    }
  },
  "logic": {
    "engine": "ble_device_scanner"
  }
})rawliteral";
    }
    return "";
}

void AppManager::init() {
    registry.clear();
    appStates.clear();
    prodMenuPage = 0;
    secMenuPage = 0;
    goodMenuPage = 0;
    badMenuPage = 0;

    auto uninstalled = getUninstalledList();
    auto isUninstalled = [&](const char* id) {
        for (const auto& u : uninstalled) {
            if (u.equalsIgnoreCase(id)) return true;
        }
        return false;
    };

    Preferences p;
    p.begin("packages", true);

    auto add = [&](const char* id, const char* name, AppCategory cat, uint16_t col, const char* icon, const char* ver, const char* desc, AppStatusFn statusFn = nullptr) {
        if (isUninstalled(id)) return;
        AppPackage pkg;
        pkg.id = id;
        pkg.name = name;
        pkg.category = cat;
        pkg.color = col;
        pkg.iconText = icon;
        pkg.pageId = DYNAMIC_PAGE_BASE + (int)registry.size();
        pkg.enabled = p.getBool(id, true);
        pkg.isCustom = false;
        pkg.version = ver;
        pkg.author = "WatchOS";
        pkg.description = desc;
        pkg.order = p.getInt((String(id) + "_ord").c_str(), (int)registry.size() + 1);
        pkg.driver = id;
        pkg.drawFn = nullptr;
        pkg.touchFn = nullptr;
        pkg.statusFn = statusFn;
        pkg.stopFn = nullptr;

        String defJson = getDefaultAppPackageJson(id);
        pkg.uiJson = jsonExtractBlock(defJson, "ui");
        pkg.logicJson = jsonExtractBlock(defJson, "logic");
        String rawJs = jsonExtractString(pkg.logicJson, "script", "");
        if (rawJs.length() == 0) rawJs = jsonExtractString(defJson, "script", "");
        pkg.jsScript = stripJsComments(rawJs);
        pkg.tickIntervalMs = jsonExtractInt(pkg.logicJson, "tick_ms", 50);
        pkg.jsInitialized = false;

        registry.push_back(pkg);

        // Auf FFat sicherstellen und alte Engine-Stub-Dateien für JS-Apps automatisch aktualisieren
        if (littleFsReady) {
            if (!FFat.exists("/apps")) FFat.mkdir("/apps");
            String path = "/apps/" + String(id) + ".app";
            bool needWrite = !FFat.exists(path);
            if (!needWrite && pkg.jsScript.length() > 0) {
                fs::File rf = FFat.open(path, "r");
                if (rf) {
                    String existingContent = rf.readString();
                    rf.close();
                    if (existingContent.indexOf("\"script\"") == -1 && existingContent.indexOf("\"engine\"") != -1) {
                        needWrite = true; // Migration von altem Engine-Verweis auf echtes JS-Paket
                    }
                }
            }
            if (needWrite && defJson.length() > 0) {
                fs::File wf = FFat.open(path, "w");
                if (wf) {
                    wf.print(defJson);
                    wf.close();
                }
            }
        }
    };

    // --- Productivity Apps ---
    add("stopwatch",   "Stoppuhr",   CAT_PRODUCTIVITY, cAcc1,      "SW", "1.1.0", "Präzisions-Stoppuhr mit Rundenzeiten (JS)", [](){ return swRunning; });
    add("timer",       "Timer",      CAT_PRODUCTIVITY, cAcc2,      "TI", "1.1.0", "Countdown-Timer mit Alarmton (JS)", [](){ return timerRunning; });
    add("mic",         "Dezibel",    CAT_PRODUCTIVITY, TFT_GREEN,  "DB", "1.0.0", "Schallpegelmesser mit I2S-Mikrofon", [](){ return micActive; });
    add("calc",        "Rechner",    CAT_PRODUCTIVITY, cFG,        "CA", "1.1.0", "Taschenrechner mit Touch-Bedienung (JS)", nullptr);
    add("jiggler",     "Jiggler",    CAT_PRODUCTIVITY, TFT_CYAN,   "MJ", "1.1.0", "USB HID Mouse Mover gegen Bildschirmschoner (JS)", [](){ return jigglerActive; });
    add("dice",        "Wuerfel",    CAT_PRODUCTIVITY, TFT_YELLOW, "D6", "1.1.0", "Würfel und Münzwurf (JS)", [](){ return diceRolling; });
    add("level",       "Waage",      CAT_PRODUCTIVITY, TFT_ORANGE, "LV", "1.0.0", "2D-Wasserwaage per BMA423 Gyro/Beschleunigung", nullptr);
    add("totp",        "2FA TOTP",   CAT_PRODUCTIVITY, 0x07FF,     "2F", "1.1.0", "Zwei-Faktor-Authenticator nach RFC 6238", nullptr);
    add("lora",        "LoRa Funk",  CAT_PRODUCTIVITY, 0x07FF,     "RF", "2.0.0", "Off-Grid P2P Messenger & Beacon auf 868MHz", [](){ return LoraManager::isBeaconActive(); });
    add("notif",       "Mitteilung", CAT_PRODUCTIVITY, 0x9DFF,     "NT", "1.0.0", "Push-Mitteilungszentrale vom Smartphone", nullptr);
    add("battery",     "Akku-Info",  CAT_PRODUCTIVITY, 0x37E0,     "BA", "1.0.0", "Detaillierte Telemetrie und Ladehistorie", nullptr);
    add("flashlight",  "Lampe",      CAT_PRODUCTIVITY, TFT_WHITE,  "FL", "1.0.0", "Helles Display-Flutlicht", nullptr);

    // --- Security Apps ---
    add("phishing",    "Phishing",   CAT_SECURITY, TFT_RED,     "EV", "1.0.0", "Captive Portal zur Demonstration von Phishing", [](){ return evilActive; });
    add("badusb",      "Ducky",      CAT_SECURITY, TFT_YELLOW,  "DU", "1.2.0", "Rubber Ducky USB HID Tastatur-Emulation", [](){ return badUsbActive; });
    add("wifispam",    "WiFi Spam",  CAT_SECURITY, TFT_RED,     "WS", "1.0.0", "Aussenden von hunderten gefälschten WiFi-Beacons", [](){ return wifiSpamActive; });
    add("blespam",     "BLE Spam",   CAT_SECURITY, TFT_MAGENTA, "BS", "1.0.0", "BLE Advertisement Flooding & Notification Popups", [](){ return bleSpamActive; });
    add("nmap",        "Nmap",       CAT_SECURITY, TFT_GREEN,   "NM", "1.0.0", "Multi-Port Netzwerk-Portscanner", [](){ return nmapActive; });
    add("deauth",      "Deauth",     CAT_SECURITY, TFT_RED,     "DA", "1.0.0", "802.11 Deauthentication Frame Generator", [](){ return deauthActive; });
    add("probemon",    "Probe Mon",  CAT_SECURITY, TFT_CYAN,    "PM", "1.0.0", "Erfassen von Probe Requests mobiler Geräte", [](){ return probeMonActive; });
    add("blescan",     "BLE Scan",   CAT_SECURITY, TFT_MAGENTA, "BC", "1.0.0", "BLE Umgebungsscanner & Device Discovery", [](){ return bleScanActive; });

    p.end();
    loadInstalledPackages();
}

void AppManager::loadInstalledPackages() {
    if (!littleFsReady) return;
    if (!FFat.exists("/apps")) FFat.mkdir("/apps");

    fs::File root = FFat.open("/apps");
    if (!root || !root.isDirectory()) return;

    fs::File file = root.openNextFile();
    while (file) {
        String fn = file.name();
        if (fn.endsWith(".app")) {
            String jsonStr = "";
            while (file.available()) jsonStr += (char)file.read();
            file.close();

            String dummyId, dummyErr;
            installPackageFromJson(jsonStr, dummyId, dummyErr);
        }
        file = root.openNextFile();
    }
}

bool AppManager::installPackageFromJson(const String &jsonStr, String &outId, String &outError) {
    String id = jsonExtractString(jsonStr, "id", "");
    id.trim();
    if (id.length() == 0) {
        outError = "Ungueltige Paketdatei: 'id' fehlt";
        return false;
    }
    outId = id;

    String name = jsonExtractString(jsonStr, "name", id);
    String catStr = jsonExtractString(jsonStr, "category", "productivity");
    AppCategory cat = (catStr.equalsIgnoreCase("security") || catStr == "1") ? CAT_SECURITY : CAT_PRODUCTIVITY;
    String colStr = jsonExtractString(jsonStr, "color", "#00e5ff");
    uint16_t color = parseHexColor(colStr);
    String icon = jsonExtractString(jsonStr, "icon", id.substring(0, min((unsigned int)2, id.length())));
    icon.toUpperCase();
    String ver = jsonExtractString(jsonStr, "version", "1.0.0");
    String author = jsonExtractString(jsonStr, "author", "Community");
    String desc = jsonExtractString(jsonStr, "description", "Installierte App");
    String driver = jsonExtractString(jsonStr, "driver", "");

    String uiJson = jsonExtractBlock(jsonStr, "ui");
    String logicJson = jsonExtractBlock(jsonStr, "logic");

    // JS-Code kann in logic.script, logic.code oder top-level "script" / "js" liegen
    String rawJs = "";
    if (logicJson.length() > 0) {
        rawJs = jsonExtractString(logicJson, "script", "");
        if (rawJs.length() == 0) rawJs = jsonExtractString(logicJson, "code", "");
    }
    if (rawJs.length() == 0) rawJs = jsonExtractString(jsonStr, "script", "");
    if (rawJs.length() == 0) rawJs = jsonExtractString(jsonStr, "js", "");
    String jsScript = stripJsComments(rawJs);
    int tickMs = (logicJson.length() > 0) ? jsonExtractInt(logicJson, "tick_ms", 50) : jsonExtractInt(jsonStr, "tick_ms", 50);

    // Wenn kein JS-Skript vorhanden ist, bekannten System-Driver zuordnen
    if (jsScript.length() == 0 && driver.length() == 0 &&
        (id == "stopwatch" || id == "timer" || id == "mic" || id == "calc" || id == "jiggler" || id == "dice" || id == "level" || id == "totp" || id == "lora" || id == "notif" || id == "battery" || id == "flashlight" || id == "phishing" || id == "badusb" || id == "wifispam" || id == "blespam" || id == "nmap" || id == "deauth" || id == "probemon" || id == "blescan")) {
        driver = id;
    }

    // Falls App in uninstalled.txt gelistet war: austragen!
    auto uninstalled = getUninstalledList();
    std::vector<String> newUninstalled;
    for (const auto& u : uninstalled) {
        if (!u.equalsIgnoreCase(id)) newUninstalled.push_back(u);
    }
    saveUninstalledList(newUninstalled);

    // Auf FFat persistieren: /apps/<id>.app
    if (littleFsReady) {
        if (!FFat.exists("/apps")) FFat.mkdir("/apps");
        fs::File f = FFat.open("/apps/" + id + ".app", "w");
        if (f) {
            f.print(jsonStr);
            f.close();
        }
    }

    // State zurücksetzen & neu initialisieren
    appStates.erase(id);

    AppPackage* existing = getAppById(id);
    if (existing) {
        existing->name = name;
        existing->category = cat;
        existing->color = color;
        existing->iconText = icon;
        existing->enabled = true;
        existing->version = ver;
        existing->author = author;
        existing->description = desc;
        existing->driver = driver;
        existing->uiJson = uiJson;
        existing->logicJson = logicJson;
        existing->jsScript = jsScript;
        existing->tickIntervalMs = tickMs;
        existing->jsInitialized = false;
        setAppEnabled(id, true);
        return true;
    }

    AppPackage pkg;
    pkg.id = id;
    pkg.name = name;
    pkg.category = cat;
    pkg.color = color;
    pkg.iconText = icon;
    pkg.pageId = DYNAMIC_PAGE_BASE + (int)registry.size();
    pkg.enabled = true;
    pkg.isCustom = true;
    pkg.version = ver;
    pkg.author = author;
    pkg.description = desc;
    pkg.driver = driver;
    pkg.uiJson = uiJson;
    pkg.logicJson = logicJson;
    pkg.jsScript = jsScript;
    pkg.tickIntervalMs = tickMs;
    pkg.jsInitialized = false;
    pkg.order = (int)registry.size() + 1;
    pkg.drawFn = nullptr;
    pkg.touchFn = nullptr;
    pkg.statusFn = nullptr;
    pkg.stopFn = nullptr;
    registry.push_back(pkg);

    setAppEnabled(id, true);
    return true;
}

bool AppManager::uninstallPackage(const String &id) {
    AppPackage* p = getAppById(id);
    if (!p) return false;

    auto uninstalled = getUninstalledList();
    bool existsInList = false;
    for (const auto& u : uninstalled) {
        if (u.equalsIgnoreCase(id)) { existsInList = true; break; }
    }
    if (!existsInList) {
        uninstalled.push_back(id);
        saveUninstalledList(uninstalled);
    }

    if (littleFsReady && FFat.exists("/apps/" + id + ".app")) {
        FFat.remove("/apps/" + id + ".app");
    }

    appStates.erase(id);

    for (auto it = registry.begin(); it != registry.end(); ++it) {
        if (it->id == id) {
            registry.erase(it);
            break;
        }
    }
    reassignDynamicPages();

    Preferences prefs;
    prefs.begin("packages", false);
    prefs.remove(id.c_str());
    prefs.remove((id + "_ord").c_str());
    prefs.end();

    return true;
}

String AppManager::getPackageAppFile(const String &id) {
    if (littleFsReady && FFat.exists("/apps/" + id + ".app")) {
        fs::File f = FFat.open("/apps/" + id + ".app", "r");
        if (f) {
            String content = "";
            while (f.available()) content += (char)f.read();
            f.close();
            if (content.length() > 10) return content;
        }
    }
    String def = getDefaultAppPackageJson(id);
    if (def.length() > 0) return def;

    AppPackage* p = getAppById(id);
    if (!p) return "";

    char hexCol[8];
    uint8_t r = ((p->color >> 11) & 0x1F) * 255 / 31;
    uint8_t g = ((p->color >> 5) & 0x3F) * 255 / 63;
    uint8_t b = (p->color & 0x1F) * 255 / 31;
    snprintf(hexCol, sizeof(hexCol), "#%02x%02x%02x", r, g, b);

    String j = "{\n";
    j += "  \"id\": \"" + jsonEscape(p->id) + "\",\n";
    j += "  \"name\": \"" + jsonEscape(p->name) + "\",\n";
    j += "  \"category\": \"" + String(p->category == CAT_SECURITY ? "security" : "productivity") + "\",\n";
    j += "  \"color\": \"" + String(hexCol) + "\",\n";
    j += "  \"icon\": \"" + jsonEscape(p->iconText) + "\",\n";
    j += "  \"version\": \"" + jsonEscape(p->version) + "\",\n";
    j += "  \"author\": \"" + jsonEscape(p->author) + "\",\n";
    j += "  \"description\": \"" + jsonEscape(p->description) + "\"";
    if (p->driver.length() > 0) j += ",\n  \"driver\": \"" + jsonEscape(p->driver) + "\"";
    if (p->uiJson.length() > 0) j += ",\n  \"ui\": " + p->uiJson;
    if (p->logicJson.length() > 0) j += ",\n  \"logic\": " + p->logicJson;
    j += "\n}\n";
    return j;
}

std::vector<AppPackage*> AppManager::getAppsByCategory(AppCategory cat, bool onlyEnabled) {
    std::vector<AppPackage*> res;
    for (size_t i = 0; i < registry.size(); i++) {
        if (registry[i].category == cat) {
            if (!onlyEnabled || registry[i].enabled) {
                res.push_back(&registry[i]);
            }
        }
    }
    std::sort(res.begin(), res.end(), [](const AppPackage* a, const AppPackage* b) {
        return a->order < b->order;
    });
    return res;
}

int AppManager::getAppCount(AppCategory cat, bool onlyEnabled) {
    int cnt = 0;
    for (const auto &pkg : registry) {
        if (pkg.category == cat) {
            if (!onlyEnabled || pkg.enabled) cnt++;
        }
    }
    return cnt;
}

AppPackage* AppManager::getAppById(const String &id) {
    for (auto &pkg : registry) {
        if (pkg.id == id) return &pkg;
    }
    return nullptr;
}

AppPackage* AppManager::getAppByPage(int pageId) {
    for (auto &pkg : registry) {
        if (pkg.pageId == pageId) return &pkg;
    }
    return nullptr;
}

void AppManager::launchApp(int pageId) {
    AppPackage* p = getAppByPage(pageId);
    if (!p) {
        slideToPage(pageId, 0, -1);
        return;
    }
    triggerVibration();
    if (hasJsScript(p)) ensureJsAppInitialized(p);
    if (p->id == "mic" || p->driver == "mic") startMic();
    if (p->category == CAT_SECURITY) {
        killBadApps();
    }
    slideToPage(p->pageId, 0, -1);
}

void AppManager::launchAppById(const String &id) {
    AppPackage* p = getAppById(id);
    if (p) launchApp(p->pageId);
}

void AppManager::setAppEnabled(const String &id, bool enabled) {
    AppPackage* p = getAppById(id);
    if (p) {
        p->enabled = enabled;
        Preferences prefs;
        prefs.begin("packages", false);
        prefs.putBool(id.c_str(), enabled);
        prefs.end();
    }
}

bool AppManager::isAppEnabled(const String &id) {
    AppPackage* p = getAppById(id);
    return p ? p->enabled : false;
}

void AppManager::setAppOrder(const String &id, int order) {
    AppPackage* p = getAppById(id);
    if (p) {
        p->order = order;
        Preferences prefs;
        prefs.begin("packages", false);
        prefs.putInt((id + "_ord").c_str(), order);
        prefs.end();
    }
}

// ============================================================================
// Dynamisches UI-Rendering & Touch-Dispatching (inkl. JS-Binding)
// ============================================================================
void AppManager::renderDynamicApp(AppPackage *app, TFT_eSprite *spr) {
    if (!app || !spr) return;
    ensureJsAppInitialized(app);

    String json = app->uiJson;
    if (json.length() == 0) {
        spr->setTextDatum(MC_DATUM);
        spr->setTextColor(app->color);
        spr->drawString(app->name, 120, 75, 4);
        spr->setTextColor(0x8BD0);
        spr->drawString("v" + app->version + " · " + app->author, 120, 108, 2);
        spr->setTextColor(0xFFFF);
        spr->drawString(app->description, 120, 132, 1);

        spr->fillRoundRect(25, 160, 190, 38, 8, app->color);
        spr->setTextColor(TFT_BLACK);
        spr->drawString("Zurueck zum Menue", 120, 179, 2);
        return;
    }

    // Titel oben zeichnen (sofern in ui.title gesetzt)
    String uiTitle = interpolateStateVars(app->id, jsonExtractString(json, "title", ""));
    if (uiTitle.length() > 0) {
        spr->setTextDatum(TC_DATUM);
        spr->setTextColor(cFG);
        spr->drawString(uiTitle, 120, 0, 2);
    }

    // Alle Objekt-Elemente { ... } in uiJson iterieren
    int pos = 1; // Erstes äußeres '{' von ui überspringen
    while ((pos = json.indexOf('{', pos)) != -1) {
        int end = findMatchingBracket(json, pos);
        if (end == -1) break;
        String elem = json.substring(pos, end + 1);
        pos = end + 1;

        String type = jsonExtractString(elem, "type", "");
        if (type.length() == 0) continue;

        String visStr = interpolateStateVars(app->id, jsonExtractString(elem, "visible", "1"));
        if (visStr == "0" || visStr.equalsIgnoreCase("false")) continue;

        String id      = jsonExtractString(elem, "id", "");
        String rawText = jsonExtractString(elem, "text", "");
        String text    = interpolateStateVars(app->id, rawText);
        int x          = jsonExtractInt(elem, "x", 120);
        int y          = jsonExtractInt(elem, "y", 50);
        int w          = jsonExtractInt(elem, "w", 200);
        int h          = jsonExtractInt(elem, "h", 36);
        int size       = jsonExtractInt(elem, "size", 2);
        String colStr  = interpolateStateVars(app->id, jsonExtractString(elem, "color", "#ffffff"));
        uint16_t col   = parseHexColor(colStr);

        if (type == "7segment") {
            int segH = jsonExtractInt(elem, "h", 22);
            int segW = jsonExtractInt(elem, "w", 12);
            int segSp = jsonExtractInt(elem, "spacing", 4);
            if (text.length() == 0 && appStates[app->id].count("timeStr")) {
                text = appStates[app->id]["timeStr"].asStr();
            }
            int bW = get7SegWidth(text, segH, segW, segSp);
            draw7SegString(*spr, text, 120 - (bW / 2), y, segH, segW, col, segSp);
        }
        else if (type == "label" || type == "text") {
            if (text.length() == 0) continue;
            String align = jsonExtractString(elem, "align", "center");
            if (align == "left") spr->setTextDatum(ML_DATUM);
            else if (align == "right") spr->setTextDatum(MR_DATUM);
            else spr->setTextDatum(MC_DATUM);
            spr->setTextColor(col);
            spr->drawString(text, x, y, size > 0 ? size : 2);
        }
        else if (type == "value") {
            String valStr = text;
            if (valStr.length() == 0 && appStates[app->id].count(id)) {
                valStr = appStates[app->id][id].asStr();
            }
            spr->setTextDatum(MC_DATUM);
            spr->setTextColor(col);
            spr->drawString(valStr, x, y, size > 0 ? size : 4);
        }
        else if (type == "lcd") {
            String bgStr = interpolateStateVars(app->id, jsonExtractString(elem, "bg", "#111822"));
            uint16_t bgCol = parseHexColor(bgStr);
            spr->fillRoundRect(x, y, w, h, 6, bgCol);
            spr->drawRoundRect(x, y, w, h, 6, col);
            spr->setTextDatum(MR_DATUM);
            spr->setTextColor(col);
            spr->drawString(text, x + w - 10, y + h / 2, size > 0 ? size : 4);
        }
        else if (type == "card" || type == "rect") {
            String bgStr = interpolateStateVars(app->id, jsonExtractString(elem, "bg", "#181c24"));
            uint16_t bgCol = parseHexColor(bgStr);
            spr->fillRoundRect(x, y, w, h, 8, bgCol);
            spr->drawRoundRect(x, y, w, h, 8, col);
            if (text.length() > 0) {
                spr->setTextDatum(MC_DATUM);
                spr->setTextColor(col);
                spr->drawString(text, x + w / 2, y + h / 2, size > 0 ? size : 2);
            }
        }
        else if (type == "button") {
            String txtColStr = interpolateStateVars(app->id, jsonExtractString(elem, "text_color", ""));
            uint16_t txtCol = txtColStr.length() >= 7 ? parseHexColor(txtColStr) : (col == 0x3186 || colStr == "#333333" || colStr == "#ff0000" ? TFT_WHITE : TFT_BLACK);
            spr->fillRoundRect(x, y, w, h, 6, col);
            spr->setTextDatum(MC_DATUM);
            spr->setTextColor(txtCol);
            spr->drawString(text, x + w / 2, y + h / 2, size > 0 ? size : 2);
        }
        else if (type == "progress" || type == "bar") {
            double val = interpolateStateVars(app->id, jsonExtractString(elem, "value", "0")).toDouble();
            double mx = max(1.0, (double)jsonExtractInt(elem, "max", 100));
            float ratio = constrain((float)(val / mx), 0.0f, 1.0f);
            spr->fillRoundRect(x, y, w, h, 4, 0x18C3);
            int fillW = (int)(w * ratio);
            if (fillW > 2) spr->fillRoundRect(x, y, fillW, h, 4, col);
        }
        else if (type == "keypad") {
            int rows = max(1, jsonExtractInt(elem, "rows", 4));
            int cols = max(1, jsonExtractInt(elem, "cols", 4));
            String keysStr = jsonExtractString(elem, "keys", "7,8,9,/,4,5,6,*,1,2,3,-,C,0,=, +");
            int cellW = w / cols;
            int cellH = h / rows;
            int kIdx = 0, kStart = 0;
            for (int r = 0; r < rows; r++) {
                for (int c = 0; c < cols; c++) {
                    int comma = keysStr.indexOf(',', kStart);
                    String kLabel = (comma == -1) ? keysStr.substring(kStart) : keysStr.substring(kStart, comma);
                    kLabel.trim();
                    kStart = (comma == -1) ? keysStr.length() : comma + 1;
                    int bx = x + c * cellW + 2;
                    int by = y + r * cellH + 2;
                    int bw = cellW - 4;
                    int bh = cellH - 4;
                    bool isOp = (kLabel == "/" || kLabel == "*" || kLabel == "-" || kLabel == "+" || kLabel == "=");
                    bool isClr = (kLabel == "C");
                    uint16_t bCol = isClr ? 0xF800 : (isOp ? 0x04FF : 0x2124);
                    spr->fillRoundRect(bx, by, bw, bh, 5, bCol);
                    spr->setTextDatum(MC_DATUM);
                    spr->setTextColor(TFT_WHITE);
                    spr->drawString(kLabel, bx + bw / 2, by + bh / 2, 2);
                    kIdx++;
                }
            }
        }
    }
}

void AppManager::handleDynamicAppTouch(AppPackage *app, int x, int touchY) {
    if (!app) return;
    ensureJsAppInitialized(app);

    if (app->uiJson.length() == 0) {
        if (touchY >= 150 && touchY <= 205) {
            triggerVibration();
            if (app->category == CAT_PRODUCTIVITY) slideToPage(Pages::GoodAppsMenu, -1, 0);
            else slideToPage(Pages::BadAppsMenu, -1, 0);
        }
        return;
    }

    String json = app->uiJson;
    int pos = 1;
    while ((pos = json.indexOf('{', pos)) != -1) {
        int end = findMatchingBracket(json, pos);
        if (end == -1) break;
        String elem = json.substring(pos, end + 1);
        pos = end + 1;

        String type = jsonExtractString(elem, "type", "");
        if (type.length() == 0) continue;

        String visStr = interpolateStateVars(app->id, jsonExtractString(elem, "visible", "1"));
        if (visStr == "0" || visStr.equalsIgnoreCase("false")) continue;

        int btnX = jsonExtractInt(elem, "x", 20);
        int btnY = jsonExtractInt(elem, "y", 0);
        int btnW = jsonExtractInt(elem, "w", 200);
        int btnH = jsonExtractInt(elem, "h", 36);

        if (type == "button") {
            if (x >= btnX && x <= (btnX + btnW) && touchY >= btnY && touchY <= (btnY + btnH)) {
                String action = jsonExtractString(elem, "action", "");
                // 1. Echte JS-Aktion ausführen, wenn Skript vorhanden
                if (app->jsScript.length() > 0 && action.length() > 0 && !action.startsWith("inc:") && !action.startsWith("dec:") && !action.startsWith("reset:") && !action.startsWith("toast:")) {
                    runJsAction(app, action);
                    lastSecond = -1;
                    return;
                }
                // 2. Abwärtskompatible Kurz-Aktionen (inc:/dec:/reset:/toast:/back)
                triggerVibration();
                if (action.startsWith("inc:")) {
                    String varId = action.substring(4);
                    appStates[app->id][varId] = JsVal(appStates[app->id][varId].asNum() + 1.0);
                } else if (action.startsWith("dec:")) {
                    String varId = action.substring(4);
                    appStates[app->id][varId] = JsVal(appStates[app->id][varId].asNum() - 1.0);
                } else if (action.startsWith("reset:")) {
                    String varId = action.substring(6);
                    appStates[app->id][varId] = JsVal(0.0);
                } else if (action.startsWith("toast:")) {
                    showToast(action.substring(6));
                } else if (action == "back") {
                    if (app->category == CAT_PRODUCTIVITY) slideToPage(Pages::GoodAppsMenu, -1, 0);
                    else slideToPage(Pages::BadAppsMenu, -1, 0);
                }
                lastSecond = -1;
                return;
            }
        }
        else if (type == "keypad") {
            if (x >= btnX && x <= (btnX + btnW) && touchY >= btnY && touchY <= (btnY + btnH)) {
                int rows = max(1, jsonExtractInt(elem, "rows", 4));
                int cols = max(1, jsonExtractInt(elem, "cols", 4));
                int colIdx = constrain((x - btnX) * cols / max(1, btnW), 0, cols - 1);
                int rowIdx = constrain((touchY - btnY) * rows / max(1, btnH), 0, rows - 1);
                int targetIdx = rowIdx * cols + colIdx;

                String keysStr = jsonExtractString(elem, "keys", "7,8,9,/,4,5,6,*,1,2,3,-,C,0,=, +");
                int kStart = 0;
                for (int k = 0; k <= targetIdx; k++) {
                    int comma = keysStr.indexOf(',', kStart);
                    String kLabel = (comma == -1) ? keysStr.substring(kStart) : keysStr.substring(kStart, comma);
                    kLabel.trim();
                    if (k == targetIdx) {
                        if (app->jsScript.length() > 0) {
                            JsVM vm(app, app->jsScript);
                            vm.callFunction("onKey", { JsVal(kLabel) });
                            syncSystemGlobalsFromState(app);
                        }
                        lastSecond = -1;
                        return;
                    }
                    if (comma == -1) break;
                    kStart = comma + 1;
                }
            }
        }
    }
}

void AppManager::resetToDefaults() {
    auto uninstalled = getUninstalledList();
    uninstalled.clear();
    saveUninstalledList(uninstalled);
    appStates.clear();

    if (littleFsReady) {
        if (!FFat.exists("/apps")) FFat.mkdir("/apps");
        static const char* appIds[] = {
            "stopwatch", "timer", "mic", "calc", "jiggler", "dice", "level", "totp",
            "lora", "notif", "battery", "flashlight", "phishing", "badusb", "wifispam",
            "blespam", "nmap", "deauth", "probemon", "blescan"
        };
        for (const char* aid : appIds) {
            fs::File f = FFat.open("/apps/" + String(aid) + ".app", "w");
            if (f) {
                f.print(getDefaultAppPackageJson(aid));
                f.close();
            }
        }
    }

    Preferences prefs;
    prefs.begin("packages", false);
    prefs.clear();
    prefs.end();

    init();
}

String AppManager::getPackagesJson() {
    String j = "{\"packages\":[";
    for (size_t i = 0; i < registry.size(); i++) {
        if (i > 0) j += ",";
        const auto &pkg = registry[i];
        bool isRunning = (pkg.statusFn != nullptr) ? pkg.statusFn() : false;
        if (!isRunning && appStates.count(pkg.id) && appStates[pkg.id].count("running")) {
            isRunning = appStates[pkg.id]["running"].asBool();
        }
        
        char hexCol[8];
        uint8_t r = ((pkg.color >> 11) & 0x1F) * 255 / 31;
        uint8_t g = ((pkg.color >> 5) & 0x3F) * 255 / 63;
        uint8_t b = (pkg.color & 0x1F) * 255 / 31;
        snprintf(hexCol, sizeof(hexCol), "#%02x%02x%02x", r, g, b);

        j += "{";
        j += "\"id\":\"" + jsonEscape(pkg.id) + "\"";
        j += ",\"name\":\"" + jsonEscape(pkg.name) + "\"";
        j += ",\"cat\":" + String((int)pkg.category);
        j += ",\"cat_name\":\"" + String(pkg.category == CAT_PRODUCTIVITY ? "Productivity" : (pkg.category == CAT_SECURITY ? "Security" : "System")) + "\"";
        j += ",\"color\":\"" + String(hexCol) + "\"";
        j += ",\"icon\":\"" + jsonEscape(pkg.iconText) + "\"";
        j += ",\"page\":" + String(pkg.pageId);
        j += ",\"enabled\":" + String(pkg.enabled ? "true" : "false");
        j += ",\"is_custom\":" + String(pkg.isCustom ? "true" : "false");
        j += ",\"has_js\":" + String(pkg.jsScript.length() > 0 ? "true" : "false");
        j += ",\"version\":\"" + jsonEscape(pkg.version) + "\"";
        j += ",\"author\":\"" + jsonEscape(pkg.author) + "\"";
        j += ",\"description\":\"" + jsonEscape(pkg.description) + "\"";
        j += ",\"running\":" + String(isRunning ? "true" : "false");
        j += ",\"order\":" + String(pkg.order);
        j += "}";
    }
    j += "]}";
    return j;
}
