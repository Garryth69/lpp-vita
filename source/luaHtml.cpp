// Otakio: HtmlNative.parse(html, NodeMeta, unescape) builds the same tree as Html.parse in the app's html.lua (tolerant
// tokenizer, implicit closes, raw text elements), in C. The Lua parser took 100-500 ms per listing page on the Vita and
// had to give frames back while it ran; this one builds the whole tree in a few milliseconds.
//
// Tree layout (exactly what html.lua builds, selectors and the Node API stay in Lua):
//   element: setmetatable({tag = "div", attrs = {...}, children = {...}, parent = <element>}, NodeMeta)
//   text:    {t = "raw text", parent = <element>[, raw = true for script/style]}
#include <string.h>
#include <string>
#include <vector>
#include <new>
extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

namespace {

inline bool isSpace(unsigned char c) { return c == ' ' || (c >= 9 && c <= 13); }
inline bool isAlpha(unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
inline bool isAlnum(unsigned char c) { return isAlpha(c) || (c >= '0' && c <= '9'); }
inline unsigned char toLower(unsigned char c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

bool inSet(const std::string &t, const char *const *set)
{
	for (int i = 0; set[i]; i++)
		if (t == set[i]) return true;
	return false;
}

const char *const VOID_TAGS[] = {"area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "param",
	"source", "track", "wbr", NULL};
const char *const RAW_TEXT[] = {"script", "style", "textarea", "title", NULL};
const char *const CLOSES_P[] = {"address", "article", "aside", "blockquote", "div", "dl", "fieldset", "footer", "form",
	"h1", "h2", "h3", "h4", "h5", "h6", "header", "hr", "main", "nav", "ol", "p", "pre", "section", "table", "ul", NULL};
const char *const LIST_STOP[] = {"ul", "ol", NULL};
const char *const TABLE_STOP[] = {"table", NULL};
const char *const P_STOP[] = {"div", "li", "td", "th", "body", NULL};

const int I_HTML = 1, I_META = 2, I_UNESCAPE = 3, I_STACK = 4, I_CHILDREN = 5;
// interned field names, pushed once (lua_rawset with these is much cheaper than lua_setfield)
const int K_TAG = 6, K_ATTRS = 7, K_CHILDREN = 8, K_PARENT = 9, K_T = 10, K_RAW = 11;

// t[key] = value on top of the stack (popped); `t` is an absolute index or -2 (the table just under the value)
inline void setKey(lua_State *L, int t, int key)
{
	if (t < 0) t = lua_gettop(L) + t + 1;
	lua_pushvalue(L, key);
	lua_insert(L, -2);
	lua_rawset(L, t);
}

struct Parser {
	lua_State *L;
	const char *s;
	size_t n;
	std::vector<std::string> tags; // tags[i] = tag of the element at depth i + 1 (depth 1 = the document)
	std::vector<int> counts;       // children count of the element at depth i + 1
	size_t pos = 0;                // next byte to read
	bool started = false;
	std::string tag;

	int depth() const { return (int)tags.size(); }

	// value on top of the Lua stack -> appended to the children of the element at `d` (popped)
	void appendTo(int d)
	{
		lua_rawgeti(L, I_CHILDREN, d);
		lua_pushvalue(L, -2);
		lua_rawseti(L, -2, ++counts[d - 1]);
		lua_pop(L, 2);
	}

	void addText(size_t from, size_t len)
	{
		if (len == 0) return;
		lua_createtable(L, 0, 2);
		lua_pushlstring(L, s + from, len);
		setKey(L, -2, K_T);
		lua_rawgeti(L, I_STACK, depth());
		setKey(L, -2, K_PARENT);
		appendTo(depth());
	}

	bool closeUntil(const std::string &tag)
	{
		for (int i = depth(); i >= 2; i--) {
			if (tags[i - 1] == tag) {
				tags.resize(i - 1);
				counts.resize(i - 1);
				return true;
			}
		}
		return false;
	}

	bool inStack(const char *tag, const char *const *stops)
	{
		for (int i = depth(); i >= 2; i--) {
			const std::string &t = tags[i - 1];
			if (t == tag) return true;
			if (stops && inSet(t, stops)) return false;
		}
		return false;
	}

	// first '>' outside quoted attribute values, from p (just after the tag name); -1: none
	long tagEnd(size_t p)
	{
		while (true) {
			size_t q = p;
			while (q < n && s[q] != '"' && s[q] != '\'' && s[q] != '>') q++;
			if (q >= n) return -1;
			char c = s[q];
			if (c == '>') return (long)q;
			// a quote opens a value only right after "=" (spaces allowed)
			long b = (long)q - 1;
			while (b >= 0 && (s[b] == ' ' || s[b] == '\t' || s[b] == '\n' || s[b] == '\r')) b--;
			if (b < 0 || s[b] != '=') {
				p = q + 1;
			} else {
				const char *close = (const char *)memchr(s + q + 1, c, n - q - 1);
				p = close ? (size_t)(close - s) + 1 : q + 1;
			}
		}
	}

	// attributes of s[a, b) -> new table on top of the stack
	void parseAttributes(size_t a, size_t b)
	{
		lua_newtable(L);
		int attrs = lua_gettop(L);
		size_t p = a;
		std::string name;
		while (p < b) {
			while (p < b && (isSpace(s[p]) || s[p] == '/')) p++;
			size_t ns = p;
			while (p < b && !isSpace(s[p]) && s[p] != '=' && s[p] != '/' && s[p] != '>' && s[p] != '"' && s[p] != '\'') p++;
			if (p == ns) break;
			name.assign(s + ns, p - ns);
			for (size_t i = 0; i < name.size(); i++) name[i] = (char)toLower((unsigned char)name[i]);
			size_t q = p;
			while (q < b && isSpace(s[q])) q++;
			if (q < b && s[q] == '=') {
				q++;
				while (q < b && isSpace(s[q])) q++;
				p = q;
				size_t vs, ve;
				if (p < b && (s[p] == '"' || s[p] == '\'')) {
					const char *close = (const char *)memchr(s + p + 1, s[p], b - p - 1);
					size_t c = close ? (size_t)(close - s) : b;
					vs = p + 1;
					ve = c;
					p = c + 1;
				} else {
					vs = p;
					while (p < b && !isSpace(s[p]) && s[p] != '>') p++;
					ve = p;
					if (p == vs) p = p + 1; // no value: Lua moved one character on
				}
				lua_pushlstring(L, name.data(), name.size());
				lua_pushvalue(L, -1);
				lua_rawget(L, attrs);
				bool unset = lua_isnil(L, -1);
				lua_pop(L, 1);
				if (unset) {
					if (memchr(s + vs, '&', ve > vs ? ve - vs : 0)) {
						lua_pushvalue(L, I_UNESCAPE);
						lua_pushlstring(L, s + vs, ve > vs ? ve - vs : 0);
						lua_call(L, 1, 1);
					} else {
						lua_pushlstring(L, s + vs, ve > vs ? ve - vs : 0);
					}
					lua_rawset(L, attrs);
				} else {
					lua_pop(L, 1);
				}
			} else {
				lua_pushlstring(L, name.data(), name.size());
				lua_pushvalue(L, -1);
				lua_rawget(L, attrs);
				bool unset = lua_isnil(L, -1);
				lua_pop(L, 1);
				if (unset) {
					lua_pushliteral(L, "");
					lua_rawset(L, attrs);
				} else {
					lua_pop(L, 1);
				}
			}
		}
	}

	// "</tag" + spaces + ">" from p; caseless also matches "</TAG >"; returns the start, `end` = index of '>'
	long findClose(size_t p, const std::string &tag, bool caseless, size_t *end)
	{
		size_t tl = tag.size();
		for (size_t i = p; i + 2 + tl < n + 1; i++) {
			if (s[i] != '<' || s[i + 1] != '/') continue;
			bool same = true;
			for (size_t k = 0; k < tl; k++) {
				unsigned char c = (unsigned char)s[i + 2 + k];
				if ((caseless ? toLower(c) : c) != (unsigned char)tag[k]) {
					same = false;
					break;
				}
			}
			if (!same) continue;
			size_t j = i + 2 + tl;
			while (j < n && isSpace(s[j])) j++;
			if (j < n && s[j] == '>') {
				*end = j;
				return (long)i;
			}
		}
		return -1;
	}

	// the document: element 1 of I_STACK
	void begin()
	{
		lua_createtable(L, 0, 4);
		lua_pushliteral(L, "#document");
		setKey(L, -2, K_TAG);
		lua_newtable(L);
		setKey(L, -2, K_ATTRS);
		lua_newtable(L);
		lua_pushvalue(L, -1);
		lua_rawseti(L, I_CHILDREN, 1);
		setKey(L, -2, K_CHILDREN);
		lua_pushvalue(L, I_META);
		lua_setmetatable(L, -2);
		lua_rawseti(L, I_STACK, 1);
		tags.push_back("#document");
		counts.push_back(0);
		pos = 0;
	}

	// reads about `budget` bytes of markup; true when the whole page is read
	bool step(size_t budget)
	{
		int doc = lua_gettop(L);
		size_t stop = pos + budget < pos ? (size_t)-1 : pos + budget;
		while (pos < n && pos < stop) {
			lua_checkstack(L, 16);
			const char *ltp = (const char *)memchr(s + pos, '<', n - pos);
			if (!ltp) {
				addText(pos, n - pos);
				pos = n;
				break;
			}
			size_t lt = (size_t)(ltp - s);
			if (lt > pos) addText(pos, lt - pos);
			unsigned char c = lt + 1 < n ? (unsigned char)s[lt + 1] : 0;
			if (c == '!') {
				if (lt + 3 < n && memcmp(s + lt, "<!--", 4) == 0) {
					size_t ce = (size_t)-1;
					for (size_t i = lt + 4; i + 2 < n; i++)
						if (s[i] == '-' && s[i + 1] == '-' && s[i + 2] == '>') {
							ce = i;
							break;
						}
					pos = ce != (size_t)-1 ? ce + 3 : n;
				} else {
					const char *ce = lt + 2 <= n ? (const char *)memchr(s + lt + 2, '>', n - lt - 2) : NULL;
					pos = ce ? (size_t)(ce - s) + 1 : n;
				}
			} else if (c == '/') {
				size_t p = lt + 2;
				while (p < n && isSpace(s[p])) p++;
				size_t ns = p;
				while (p < n && (isAlnum(s[p]) || s[p] == ':' || s[p] == '-')) p++;
				const char *gt = p > ns ? (const char *)memchr(s + p, '>', n - p) : NULL;
				if (gt) {
					tag.assign(s + ns, p - ns);
					for (size_t i = 0; i < tag.size(); i++) tag[i] = (char)toLower((unsigned char)tag[i]);
					closeUntil(tag);
					pos = (size_t)(gt - s) + 1;
				} else {
					addText(lt, 1);
					pos = lt + 1;
				}
			} else {
				long e = -1;
				size_t te = lt + 1;
				if (isAlpha(c)) {
					te = lt + 2;
					while (te < n && (isAlnum(s[te]) || s[te] == ':' || s[te] == '-')) te++;
					e = tagEnd(te);
				}
				if (e < 0) {
					addText(lt, 1);
					pos = lt + 1;
				} else {
					tag.assign(s + lt + 1, te - lt - 1);
					for (size_t i = 0; i < tag.size(); i++) tag[i] = (char)toLower((unsigned char)tag[i]);
					pos = (size_t)e + 1;
					bool selfClosing = (size_t)e > te && s[e - 1] == '/';
					// implicit closes
					if (tag == "li" && inStack("li", LIST_STOP)) {
						closeUntil("li");
					} else if ((tag == "td" || tag == "th") && (inStack("td", TABLE_STOP) || inStack("th", TABLE_STOP))) {
						if (!closeUntil("td")) closeUntil("th");
					} else if (tag == "tr" && inStack("tr", TABLE_STOP)) {
						closeUntil("tr");
					} else if (tag == "option" && tags.back() == "option") {
						closeUntil("option");
					} else if (inSet(tag, CLOSES_P) && inStack("p", P_STOP)) {
						closeUntil("p");
					}
					// the element
					lua_createtable(L, 0, 4);
					int el = lua_gettop(L);
					lua_pushlstring(L, tag.data(), tag.size());
					setKey(L, el, K_TAG);
					parseAttributes(te, (size_t)e);
					setKey(L, el, K_ATTRS);
					lua_newtable(L);
					int children = lua_gettop(L);
					lua_pushvalue(L, children);
					setKey(L, el, K_CHILDREN);
					lua_rawgeti(L, I_STACK, depth());
					setKey(L, el, K_PARENT);
					lua_pushvalue(L, I_META);
					lua_setmetatable(L, el);
					lua_pushvalue(L, el);
					appendTo(depth());
					if (inSet(tag, RAW_TEXT) && !selfClosing) {
						size_t ce2 = 0;
						long cs = findClose(pos, tag, false, &ce2);
						if (cs < 0) cs = findClose(pos, tag, true, &ce2);
						size_t rawEnd = cs < 0 ? n : (size_t)cs;
						if (rawEnd > pos) {
							lua_createtable(L, 0, 3);
							lua_pushlstring(L, s + pos, rawEnd - pos);
							setKey(L, -2, K_T);
							lua_pushvalue(L, el);
							setKey(L, -2, K_PARENT);
							if (tag == "script" || tag == "style") {
								lua_pushboolean(L, 1);
								setKey(L, -2, K_RAW);
							}
							lua_rawseti(L, children, 1);
						}
						pos = cs < 0 ? n : ce2 + 1;
					} else if (!(inSet(tag, VOID_TAGS) || selfClosing)) {
						tags.push_back(tag);
						counts.push_back(0);
						lua_pushvalue(L, el);
						lua_rawseti(L, I_STACK, depth());
						lua_pushvalue(L, children);
						lua_rawseti(L, I_CHILDREN, depth());
					}
					lua_settop(L, doc);
				}
			}
		}
		lua_settop(L, doc);
		return pos >= n;
	}
};

const char *PARSER_META = "HtmlNativeParser";

// keeps on the stack, at I_HTML..K_RAW: html, meta, unescape, element stack, children stack, field names
void pushState(lua_State *L, int env)
{
	for (int i = 1; i <= 5; i++) lua_rawgeti(L, env, i);
	lua_pushliteral(L, "tag");
	lua_pushliteral(L, "attrs");
	lua_pushliteral(L, "children");
	lua_pushliteral(L, "parent");
	lua_pushliteral(L, "t");
	lua_pushliteral(L, "raw");
}

// HtmlNative.new(html, NodeMeta, unescape) -> parser; parser:step([bytes]) -> the document once it is all read, else nil.
// Steps of ~32 KB let a coroutine give the frame back between them, as html.lua's parser did.
int lua_newparser(lua_State *L)
{
	luaL_checktype(L, 2, LUA_TTABLE);
	luaL_checktype(L, 3, LUA_TFUNCTION);
	lua_settop(L, 3);
	void *mem = lua_newuserdata(L, sizeof(Parser));
	new (mem) Parser();
	luaL_getmetatable(L, PARSER_META);
	lua_setmetatable(L, 4);
	lua_createtable(L, 5, 0);
	if (lua_type(L, 1) == LUA_TSTRING) lua_pushvalue(L, 1); else lua_pushliteral(L, "");
	lua_rawseti(L, 5, 1);
	lua_pushvalue(L, 2);
	lua_rawseti(L, 5, 2);
	lua_pushvalue(L, 3);
	lua_rawseti(L, 5, 3);
	lua_newtable(L);
	lua_rawseti(L, 5, 4);
	lua_newtable(L);
	lua_rawseti(L, 5, 5);
	lua_setfenv(L, 4);
	return 1;
}

int lua_parserstep(lua_State *L)
{
	Parser *p = (Parser *)luaL_checkudata(L, 1, PARSER_META);
	lua_Integer budget = luaL_optinteger(L, 2, 0x3FFFFFFF);
	if (budget < 1) budget = 1;
	lua_settop(L, 1);
	lua_getfenv(L, 1);
	lua_checkstack(L, 32);
	pushState(L, 2);
	// html, meta, unescape, element stack, children stack and the names at I_HTML..K_RAW (the caller keeps the parser)
	lua_remove(L, 1);
	lua_remove(L, 1);
	p->L = L;
	p->s = lua_tolstring(L, I_HTML, &p->n);
	if (!p->started) {
		p->begin();
		p->started = true;
	}
	if (!p->step((size_t)budget)) return 0;
	lua_rawgeti(L, I_STACK, 1);
	return 1;
}

int lua_parsergc(lua_State *L)
{
	Parser *p = (Parser *)luaL_checkudata(L, 1, PARSER_META);
	p->~Parser();
	return 0;
}

// HtmlNative.parse(html, NodeMeta, unescape): the whole page at once
int lua_parsehtml(lua_State *L)
{
	lua_settop(L, 3);
	lua_pushcfunction(L, lua_newparser);
	lua_insert(L, 1);
	lua_call(L, 3, 1);
	lua_pushcfunction(L, lua_parserstep);
	lua_pushvalue(L, 1);
	lua_call(L, 1, 1);
	return 1;
}

const luaL_Reg HtmlNative_functions[] = {
	{"new", lua_newparser},
	{"parse", lua_parsehtml},
	{0, 0}
};

} // namespace

void luaHtml_init(lua_State *L)
{
	luaL_newmetatable(L, PARSER_META);
	lua_pushcfunction(L, lua_parsergc);
	lua_setfield(L, -2, "__gc");
	lua_newtable(L);
	lua_pushcfunction(L, lua_parserstep);
	lua_setfield(L, -2, "step");
	lua_setfield(L, -2, "__index");
	lua_pop(L, 1);
	lua_newtable(L);
	luaL_setfuncs(L, HtmlNative_functions, 0);
	lua_setglobal(L, "HtmlNative");
}
