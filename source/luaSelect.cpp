// Otakio: HtmlNative.selector, the CSS selector matching of the app's html.lua in C. Parsing pages already ran in C
// (luaHtml.cpp), but every select() still walked the tree in Lua, dozens of times per page.
//
// The first select on a tree indexes it in C (in steps): each element's tag, parent, previous element sibling,
// position among its siblings, subtree range and attributes, in document order. Every select on that tree then runs
// on the index with no Lua table reads; html.lua's trees are never changed after parsing. The index is kept on the
// tree's top node and goes away with it.
//
// The selector is still parsed by html.lua (parseSelectorGroup, cached); this file compiles that parsed form into C
// structures and runs it: the same walk order, the same tests, the same results as html.lua's selectCompiled,
// matchSteps and matchCompound. The few tests that need Lua patterns or a node's text (:contains, :containsOwn,
// :matches, :matchesOwn, :nth-child with an expression) call back into Lua through the `test` function the caller
// passes.
//
// HtmlNative.selector(root, group, firstOnly, ElementsMeta, test) -> selector
// selector:step([nodes]) -> true, result when done (an Elements list, or the first element / nil when firstOnly);
//                           false while there is more; nil, error when `test` failed; nil, nil when the root isn't
//                           an element of an indexed tree (the caller then uses the Lua walk)
// Steps let a coroutine give the frame back between them, like the Lua walk.
//
// `test(node, t)` returns true/false, or nil, error.
//
// HashNative.sha256(bytes) -> the lowercase hex SHA-256 of a string (sha256.lua in C).
//
// Strings in LuaJIT are interned, so two strings with the same bytes are the same object: tags, attribute names and
// whole values are compared by pointer.
#include <stdlib.h>
#include <string.h>
#include <new>
#include <string>
#include <vector>
#include <unordered_map>
extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

namespace {

const char *SELECTOR_META = "Otakio.HtmlSelector";
const char *INDEX_META = "Otakio.HtmlIndex";
const char *INDEX_KEY = "__selectorIndex"; // field of the tree's top node

// selector environment
const int E_ROOT = 1, E_RESULTS = 2, E_TEST = 3, E_TESTS = 4, E_STRS = 5, E_INDEX = 6;
// index environment
const int X_NODES = 1;

// --- the tree index

struct Attr {
	const char *name;
	const char *val; // nullptr when the value isn't a string
	size_t len;
};

struct Frame {
	int node;     // index of the element whose children are being read
	int child;    // children read so far
	int lastEl;   // last element child seen (previous sibling of the next one)
	int count;    // element children seen
};

struct Index {
	std::vector<int> parent, end, prevEl, elIdx, elTotal, attrStart;
	std::vector<const char *> tag;
	std::vector<unsigned char> flags; // F_DOC, F_NOTEMPTY
	std::vector<Attr> attrs;
	std::unordered_map<const void *, int> pos;
	std::vector<Frame> frames; // the build, between steps
	bool complete = false;
	bool started = false;
	const char *docTag = nullptr, *classKey = nullptr, *idKey = nullptr;
};
const unsigned char F_DOC = 1, F_NOTEMPTY = 2;

inline bool isspaceLua(unsigned char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

// keys used while indexing, pushed on the stack by the caller at these slots
struct Keys {
	int tag, children, isDoc, t, attrs;
};

// records the element on top of the stack (its tag string already read) and returns its index
int addElement(lua_State *L, Index *x, int nodes, const Keys &k, const char *tag, int parent)
{
	int el = lua_gettop(L);
	int i = (int)x->tag.size();
	lua_pushvalue(L, el);
	lua_rawseti(L, nodes, i + 1);
	x->pos[lua_topointer(L, el)] = i;
	x->tag.push_back(tag);
	x->parent.push_back(parent);
	x->end.push_back(i + 1);
	x->prevEl.push_back(-1);
	x->elIdx.push_back(1);
	x->elTotal.push_back(1);
	unsigned char f = 0;
	if (tag == x->docTag) {
		lua_pushvalue(L, k.isDoc);
		lua_rawget(L, el);
		if (lua_toboolean(L, -1)) f |= F_DOC;
		lua_pop(L, 1);
	}
	x->flags.push_back(f);
	x->attrStart.push_back((int)x->attrs.size());
	lua_pushvalue(L, k.attrs);
	lua_rawget(L, el);
	if (lua_istable(L, -1)) {
		lua_pushnil(L);
		while (lua_next(L, -2)) {
			if (lua_type(L, -2) == LUA_TSTRING) {
				Attr a;
				a.name = lua_tostring(L, -2);
				a.len = 0;
				a.val = lua_type(L, -1) == LUA_TSTRING ? lua_tolstring(L, -1, &a.len) : nullptr;
				x->attrs.push_back(a);
			}
			lua_pop(L, 1);
		}
	}
	lua_pop(L, 1);
	return i;
}

// indexes up to `budget` elements; true when the index is complete. `top` is the tree's top node.
bool buildIndex(lua_State *L, Index *x, int nodes, int top, const Keys &k, long &budget)
{
	if (!x->started) {
		x->started = true;
		lua_pushvalue(L, top);
		lua_pushvalue(L, k.tag);
		lua_rawget(L, top);
		const char *tag = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : nullptr;
		lua_pop(L, 1);
		int i = addElement(L, x, nodes, k, tag, -1);
		lua_pop(L, 1);
		x->frames.push_back(Frame{i, 0, -1, 0});
	}
	int base = lua_gettop(L);
	while (!x->frames.empty()) {
		Frame &f = x->frames.back();
		lua_settop(L, base);
		lua_rawgeti(L, nodes, f.node + 1);
		lua_pushvalue(L, k.children);
		lua_rawget(L, -2);
		bool done = true;
		if (lua_istable(L, -1)) {
			lua_rawgeti(L, -1, ++f.child);
			done = lua_isnil(L, -1);
		}
		if (done) {
			x->end[f.node] = (int)x->tag.size();
			x->frames.pop_back();
			continue;
		}
		if (!lua_istable(L, -1)) continue;
		lua_pushvalue(L, k.tag);
		lua_rawget(L, -2);
		bool isEl = lua_toboolean(L, -1) != 0;
		const char *tag = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : nullptr;
		lua_pop(L, 1);
		if (!isEl) {
			// a text node: html.lua's :empty fails on any non-blank text
			if (!(x->flags[f.node] & F_NOTEMPTY)) {
				lua_pushvalue(L, k.t);
				lua_rawget(L, -2);
				size_t n = 0;
				const char *s = lua_type(L, -1) == LUA_TSTRING ? lua_tolstring(L, -1, &n) : nullptr;
				for (size_t j = 0; s && j < n; j++) {
					if (!isspaceLua((unsigned char)s[j])) {
						x->flags[f.node] |= F_NOTEMPTY;
						break;
					}
				}
			}
			continue;
		}
		int parent = f.node;
		x->flags[parent] |= F_NOTEMPTY;
		int i = addElement(L, x, nodes, k, tag, parent);
		Frame &pf = x->frames.back(); // addElement doesn't touch frames, but keep the reference honest
		x->prevEl[i] = pf.lastEl;
		pf.lastEl = i;
		x->elIdx[i] = ++pf.count;
		x->frames.push_back(Frame{i, 0, -1, 0});
		if (--budget <= 0 && !x->frames.empty()) {
			lua_settop(L, base);
			return false;
		}
	}
	lua_settop(L, base);
	// element counts per parent
	size_t n = x->tag.size();
	std::vector<int> count(n, 0);
	for (size_t i = 0; i < n; i++) if (x->parent[i] >= 0) count[x->parent[i]]++;
	for (size_t i = 0; i < n; i++) if (x->parent[i] >= 0) x->elTotal[i] = count[x->parent[i]];
	x->complete = true;
	x->frames.shrink_to_fit();
	return true;
}

int lua_indexgc(lua_State *L)
{
	Index *x = (Index *)luaL_checkudata(L, 1, INDEX_META);
	x->~Index();
	return 0;
}

// --- compiled selectors

enum Kind { K_CLASS, K_ID, K_ATTR, K_NOT, K_HAS, K_NTH, K_NTHLAST, K_EMPTY, K_LUA };
enum Op { OP_NONE, OP_EQ, OP_PRE, OP_SUF, OP_SUB, OP_WORD, OP_DASH, OP_NE, OP_OTHER };
enum Filter { F_NONE, F_EQ, F_LT, F_GT, F_OTHER };

struct Str {
	const char *p = nullptr;
	size_t n = 0;
};

struct Group;
struct Test {
	Kind kind = K_LUA;
	Str a, b;
	Op op = OP_NONE;
	lua_Number num = 0;
	int ref = 0; // index of the test table in the env's tests list (for `test`)
	Group *sub = nullptr;
};
struct Step {
	Str tag; // p == nullptr: any
	char comb = ' ';
	std::vector<Test> tests;
};
struct Part {
	std::vector<Step> steps;
	bool relChild = false; // a leading ">"
	Filter filter = F_NONE;
	lua_Number filterN = 0;
};
struct Group {
	std::vector<Part> parts;
};

struct Selector {
	bool firstOnly = false;
	bool started = false;
	bool done = false;
	bool failed = false;
	bool noIndex = false;
	size_t phase = 0; // 0: the plain parts; p > 0: the part p - 1, which has an :eq/:lt/:gt filter
	int root = -1;    // root's index in the tree index
	int next = 0;     // next element index to visit in the walk
	int resultCount = 0;
	Group *group = nullptr;
	std::vector<Group *> owned;
	std::vector<int> matched;
	std::string err;
	~Selector()
	{
		for (Group *g : owned) delete g;
	}
};

struct Compiler {
	lua_State *L;
	Selector *s;
	int strs, tests; // env tables being filled (they keep the strings alive)

	Str str(int i)
	{
		Str r;
		if (lua_type(L, i) != LUA_TSTRING) return r;
		r.p = lua_tolstring(L, i, &r.n);
		lua_pushvalue(L, i);
		lua_rawseti(L, strs, (int)lua_objlen(L, strs) + 1);
		return r;
	}

	Str field(int t, int k)
	{
		lua_rawgeti(L, t, k);
		Str r = str(lua_gettop(L));
		lua_pop(L, 1);
		return r;
	}

	Group *group(int g)
	{
		lua_checkstack(L, 16);
		Group *out = new Group();
		s->owned.push_back(out);
		int n = (int)lua_objlen(L, g);
		for (int i = 1; i <= n; i++) {
			lua_rawgeti(L, g, i);
			int steps = lua_gettop(L);
			Part part;
			if (lua_istable(L, steps)) {
				lua_getfield(L, steps, "relative");
				const char *rel = lua_tostring(L, -1);
				part.relChild = rel && strcmp(rel, ">") == 0;
				lua_pop(L, 1);
				lua_getfield(L, steps, "filter");
				if (lua_istable(L, -1)) {
					lua_rawgeti(L, -1, 1);
					lua_rawgeti(L, -2, 2);
					const char *k = lua_tostring(L, -2);
					part.filter = !k ? F_OTHER : strcmp(k, "eq") == 0 ? F_EQ : strcmp(k, "lt") == 0 ? F_LT : strcmp(k, "gt") == 0 ? F_GT : F_OTHER;
					part.filterN = lua_tonumber(L, -1);
					lua_pop(L, 2);
				} else if (!lua_isnil(L, -1)) {
					part.filter = F_OTHER;
				}
				lua_pop(L, 1);
				int ns = (int)lua_objlen(L, steps);
				for (int k = 1; k <= ns; k++) {
					lua_rawgeti(L, steps, k);
					part.steps.push_back(step(lua_gettop(L)));
					lua_pop(L, 1);
				}
			}
			out->parts.push_back(std::move(part));
			lua_settop(L, steps - 1);
		}
		return out;
	}

	Step step(int st)
	{
		Step out;
		if (!lua_istable(L, st)) return out;
		lua_getfield(L, st, "comb");
		const char *c = lua_tostring(L, -1);
		out.comb = c && c[0] ? c[0] : ' ';
		lua_pop(L, 1);
		lua_getfield(L, st, "tests");
		int tt = lua_gettop(L);
		if (lua_istable(L, tt)) {
			lua_getfield(L, tt, "tag");
			out.tag = str(lua_gettop(L));
			lua_pop(L, 1);
			int n = (int)lua_objlen(L, tt);
			for (int k = 1; k <= n; k++) {
				lua_rawgeti(L, tt, k);
				int t = lua_gettop(L);
				Test test;
				if (compileTest(t, test)) out.tests.push_back(test);
				lua_settop(L, t - 1);
			}
		}
		lua_settop(L, st);
		return out;
	}

	// false for the tests html.lua's matchCompound ignores (eq/lt/gt, unknown kinds)
	bool compileTest(int t, Test &test)
	{
		if (!lua_istable(L, t)) return false;
		lua_rawgeti(L, t, 1);
		const char *kind = lua_tostring(L, -1);
		lua_pop(L, 1);
		if (!kind) return false;
		if (strcmp(kind, "class") == 0 || strcmp(kind, "id") == 0) {
			test.kind = kind[0] == 'c' ? K_CLASS : K_ID;
			test.a = field(t, 2);
			if (!test.a.p) return asLua(t, test);
		} else if (strcmp(kind, "attr") == 0) {
			test.kind = K_ATTR;
			test.a = field(t, 2);
			if (!test.a.p) return asLua(t, test);
			lua_rawgeti(L, t, 3);
			const char *op = lua_tostring(L, -1);
			lua_pop(L, 1);
			if (op) {
				test.b = field(t, 4);
				if (!test.b.p) return asLua(t, test);
				test.op = strcmp(op, "=") == 0 ? OP_EQ : strcmp(op, "^=") == 0 ? OP_PRE : strcmp(op, "$=") == 0 ? OP_SUF
					: strcmp(op, "*=") == 0 ? OP_SUB : strcmp(op, "~=") == 0 ? OP_WORD : strcmp(op, "|=") == 0 ? OP_DASH
					: strcmp(op, "!=") == 0 ? OP_NE : OP_OTHER;
			}
		} else if (strcmp(kind, "not") == 0 || strcmp(kind, "has") == 0) {
			test.kind = kind[0] == 'n' ? K_NOT : K_HAS;
			lua_rawgeti(L, t, 2);
			if (!lua_istable(L, -1)) {
				lua_pop(L, 1);
				return asLua(t, test);
			}
			test.sub = group(lua_gettop(L));
			lua_pop(L, 1);
		} else if (strcmp(kind, "nth") == 0 || strcmp(kind, "nthlast") == 0) {
			lua_rawgeti(L, t, 2);
			bool num = lua_type(L, -1) == LUA_TNUMBER;
			test.num = lua_tonumber(L, -1);
			lua_pop(L, 1);
			if (!num) return asLua(t, test);
			test.kind = kind[3] == 'l' ? K_NTHLAST : K_NTH;
		} else if (strcmp(kind, "empty") == 0) {
			test.kind = K_EMPTY;
		} else if (strcmp(kind, "contains") == 0 || strcmp(kind, "containsown") == 0 || strcmp(kind, "matches") == 0 ||
		           strcmp(kind, "matchesown") == 0) {
			return asLua(t, test);
		} else {
			return false;
		}
		return true;
	}

	bool asLua(int t, Test &test)
	{
		test.kind = K_LUA;
		test.sub = nullptr;
		lua_pushvalue(L, t);
		test.ref = (int)lua_objlen(L, tests) + 1;
		lua_rawseti(L, tests, test.ref);
		return true;
	}
};

// --- matching on the index

struct Run {
	lua_State *L;
	Index *x;
	Selector *s;
	int nodes, testFn, testList; // stack slots for the Lua callbacks
};

const char *findPlain(const char *h, size_t hn, const char *n, size_t nn)
{
	if (nn == 0) return h;
	if (nn > hn) return nullptr;
	const char *end = h + hn - nn;
	for (const char *p = h; p <= end; p++) {
		p = (const char *)memchr(p, n[0], end - p + 1);
		if (!p) return nullptr;
		if (memcmp(p, n, nn) == 0) return p;
	}
	return nullptr;
}

// a whitespace-separated word of `s` equals `w` (gmatch "%S+")
bool hasWord(const char *s, size_t n, const char *w, size_t wn)
{
	size_t i = 0;
	while (i < n) {
		while (i < n && isspaceLua((unsigned char)s[i])) i++;
		size_t st = i;
		while (i < n && !isspaceLua((unsigned char)s[i])) i++;
		if (i > st && i - st == wn && memcmp(s + st, w, wn) == 0) return true;
	}
	return false;
}

inline const Attr *findAttr(const Index *x, int i, const char *name)
{
	int a = x->attrStart[i];
	int e = i + 1 < (int)x->attrStart.size() ? x->attrStart[i + 1] : (int)x->attrs.size();
	for (; a < e; a++) {
		if (x->attrs[a].name == name) return &x->attrs[a];
	}
	return nullptr;
}

bool matchSteps(Run &r, int node, const Part &part, int k, int root);
bool matchGroupAll(Run &r, int node, const Group &g);
bool hasMatch(Run &r, int node, const Group &g);

// test(node, t); false on error (the error is kept and the walk stops)
bool callTest(Run &r, int node, int ref)
{
	if (r.s->failed) return false;
	lua_State *L = r.L;
	lua_checkstack(L, 8);
	lua_pushvalue(L, r.testFn);
	lua_rawgeti(L, r.nodes, node + 1);
	lua_rawgeti(L, r.testList, ref);
	lua_call(L, 2, 2);
	bool ok = lua_toboolean(L, -2) != 0;
	if (lua_isnil(L, -2)) {
		const char *e = lua_tostring(L, -1);
		if (e) {
			r.s->failed = true;
			r.s->err = e;
		}
	}
	lua_pop(L, 2);
	return ok;
}

bool matchCompound(Run &r, int i, const Step &st)
{
	const Index *x = r.x;
	if (x->flags[i] & F_DOC) return false;
	if (st.tag.p && st.tag.p != x->tag[i]) return false;
	for (const Test &t : st.tests) {
		bool ok = true;
		switch (t.kind) {
		case K_CLASS: {
			const Attr *a = findAttr(x, i, x->classKey);
			ok = a && a->val && hasWord(a->val, a->len, t.a.p, t.a.n);
			break;
		}
		case K_ID: {
			const Attr *a = findAttr(x, i, x->idKey);
			ok = a && a->val == t.a.p;
			break;
		}
		case K_ATTR: {
			const Attr *a = findAttr(x, i, t.a.p);
			if (!a) { ok = false; break; }
			if (t.op == OP_NONE || t.op == OP_OTHER) break;
			const char *v = a->val, *s = t.b.p;
			size_t vn = a->len, sn = t.b.n;
			if (!v) { ok = t.op == OP_NE; break; } // a non-string value never equals the string
			switch (t.op) {
			case OP_EQ: ok = v == s; break;
			case OP_NE: ok = v != s; break;
			case OP_PRE: ok = sn > 0 && sn <= vn && memcmp(v, s, sn) == 0; break;
			case OP_SUF: ok = sn > 0 && sn <= vn && memcmp(v + vn - sn, s, sn) == 0; break;
			case OP_SUB: ok = sn > 0 && findPlain(v, vn, s, sn) != nullptr; break;
			case OP_WORD: ok = hasWord(v, vn, s, sn); break;
			case OP_DASH: ok = v == s || (vn >= sn + 1 && memcmp(v, s, sn) == 0 && v[sn] == '-'); break;
			default: break;
			}
			break;
		}
		case K_NOT: ok = !matchGroupAll(r, i, *t.sub); break;
		case K_HAS: ok = hasMatch(r, i, *t.sub); break;
		case K_NTH: ok = (lua_Number)x->elIdx[i] == t.num; break;
		case K_NTHLAST: ok = (lua_Number)(x->elTotal[i] - x->elIdx[i] + 1) == t.num; break;
		case K_EMPTY: ok = !(x->flags[i] & F_NOTEMPTY); break;
		case K_LUA: ok = callTest(r, i, t.ref); break;
		}
		if (!ok || r.s->failed) return false;
	}
	return true;
}

// html.lua's matchSteps with idx = k + 1; root -1 stands for nil (no root), like a missing parent
bool matchSteps(Run &r, int node, const Part &part, int k, int root)
{
	if (r.s->failed) return false;
	const Step &st = part.steps[k];
	if (!matchCompound(r, node, st)) return false;
	const Index *x = r.x;
	if (k == 0) return !part.relChild || x->parent[node] == root;
	switch (st.comb) {
	case ' ':
		for (int p = x->parent[node]; p >= 0 && p != root; p = x->parent[p]) {
			if (matchSteps(r, p, part, k - 1, root)) return true;
		}
		return false;
	case '>': {
		int p = x->parent[node];
		return p >= 0 && p != root && matchSteps(r, p, part, k - 1, root);
	}
	case '+': {
		int p = x->prevEl[node];
		return p >= 0 && matchSteps(r, p, part, k - 1, root);
	}
	case '~':
		for (int p = x->prevEl[node]; p >= 0; p = x->prevEl[p]) {
			if (matchSteps(r, p, part, k - 1, root)) return true;
		}
		return false;
	}
	return false;
}

// html.lua's matchGroup (:not): every part, filters or not, with no root
bool matchGroupAll(Run &r, int node, const Group &g)
{
	for (const Part &part : g.parts) {
		if (!part.steps.empty() && matchSteps(r, node, part, (int)part.steps.size() - 1, -1)) return true;
	}
	return false;
}

inline bool matchPlain(Run &r, int node, const Group &g, int root)
{
	for (const Part &part : g.parts) {
		if (part.filter == F_NONE && !part.steps.empty() && matchSteps(r, node, part, (int)part.steps.size() - 1, root))
			return true;
	}
	return false;
}

inline bool filterKeeps(const Part &part, int i)
{
	switch (part.filter) {
	case F_EQ: return (lua_Number)i == part.filterN;
	case F_LT: return (lua_Number)i < part.filterN;
	case F_GT: return (lua_Number)i > part.filterN;
	default: return false;
	}
}

// :has(group) on the node: html.lua's selectFirstCompiled(node, group) ~= nil (the walk is the node's subtree)
bool hasMatch(Run &r, int node, const Group &g)
{
	int end = r.x->end[node];
	for (int i = node + 1; i < end; i++) {
		if (matchPlain(r, i, g, node)) return true;
		if (r.s->failed) return false;
	}
	for (const Part &part : g.parts) {
		if (part.filter == F_NONE || part.steps.empty()) continue;
		int last = (int)part.steps.size() - 1;
		int count = 0;
		for (int i = node + 1; i < end; i++) {
			if (matchSteps(r, i, part, last, node) && filterKeeps(part, ++count)) return true;
			if (r.s->failed) return false;
		}
	}
	return false;
}

// --- the selector object

int lua_newselector(lua_State *L)
{
	luaL_checktype(L, 1, LUA_TTABLE);
	luaL_checktype(L, 2, LUA_TTABLE);
	luaL_checktype(L, 5, LUA_TFUNCTION);
	lua_settop(L, 5);
	void *mem = lua_newuserdata(L, sizeof(Selector));
	Selector *s = new (mem) Selector();
	s->firstOnly = lua_toboolean(L, 3) != 0;
	luaL_getmetatable(L, SELECTOR_META);
	lua_setmetatable(L, 6);
	lua_createtable(L, 6, 0);
	lua_pushvalue(L, 1);
	lua_rawseti(L, 7, E_ROOT);
	lua_newtable(L);
	if (lua_istable(L, 4)) {
		lua_pushvalue(L, 4);
		lua_setmetatable(L, -2);
	}
	lua_rawseti(L, 7, E_RESULTS);
	lua_pushvalue(L, 5);
	lua_rawseti(L, 7, E_TEST);
	lua_newtable(L);
	lua_pushvalue(L, -1);
	lua_rawseti(L, 7, E_TESTS); // 8
	lua_newtable(L);
	lua_pushvalue(L, -1);
	lua_rawseti(L, 7, E_STRS); // 9
	Compiler c{L, s, 9, 8};
	s->group = c.group(2);
	lua_settop(L, 7);
	lua_setfenv(L, 6);
	return 1;
}

int finish(lua_State *L, Selector *s, int results)
{
	s->done = true;
	lua_pushboolean(L, 1);
	if (s->firstOnly) lua_rawgeti(L, results, 1); else lua_pushvalue(L, results);
	return 2;
}

// the index of the tree `root` belongs to (pushed), created if needed; nullptr when root isn't a table
Index *pushIndex(lua_State *L, int root, const Keys &k)
{
	if (!lua_istable(L, root)) return nullptr;
	lua_pushliteral(L, "parent");
	int parentKey = lua_gettop(L);
	lua_pushvalue(L, root);
	int top = lua_gettop(L);
	for (int guard = 0; guard < 100000; guard++) {
		lua_pushvalue(L, parentKey);
		lua_rawget(L, top);
		if (!lua_istable(L, -1)) {
			lua_pop(L, 1);
			break;
		}
		lua_replace(L, top);
	}
	lua_pushstring(L, INDEX_KEY);
	lua_rawget(L, top);
	Index *x = nullptr;
	if (lua_type(L, -1) == LUA_TUSERDATA && lua_getmetatable(L, -1)) {
		luaL_getmetatable(L, INDEX_META);
		if (lua_rawequal(L, -1, -2)) x = (Index *)lua_touserdata(L, -3);
		lua_pop(L, 2);
	}
	if (!x) {
		lua_pop(L, 1);
		void *mem = lua_newuserdata(L, sizeof(Index));
		x = new (mem) Index();
		luaL_getmetatable(L, INDEX_META);
		lua_setmetatable(L, -2);
		lua_createtable(L, 1, 0);
		lua_newtable(L);
		lua_rawseti(L, -2, X_NODES);
		lua_setfenv(L, -2);
		lua_pushstring(L, INDEX_KEY);
		lua_pushvalue(L, -2);
		lua_rawset(L, top);
	}
	(void)k;
	// [parentKey, top, index] -> [index, top]
	lua_replace(L, parentKey);
	return x;
}

int lua_selectorstep(lua_State *L)
{
	Selector *s = (Selector *)luaL_checkudata(L, 1, SELECTOR_META);
	long budget = (long)luaL_optinteger(L, 2, 0x3FFFFFFF);
	if (budget < 1) budget = 1;
	if (s->failed) {
		lua_pushnil(L);
		lua_pushstring(L, s->err.c_str());
		return 2;
	}
	if (s->noIndex) {
		lua_pushnil(L);
		lua_pushnil(L);
		return 2;
	}
	lua_settop(L, 1);
	lua_checkstack(L, 64);
	lua_getfenv(L, 1);           // 2
	int env = 2;
	lua_rawgeti(L, env, E_ROOT);    // 3
	lua_rawgeti(L, env, E_RESULTS); // 4
	lua_rawgeti(L, env, E_TEST);    // 5
	lua_rawgeti(L, env, E_TESTS);   // 6
	const int root = 3, results = 4, testFn = 5, testList = 6;
	if (s->done) return finish(L, s, results);
	lua_pushliteral(L, "tag");        // 7
	lua_pushliteral(L, "children");   // 8
	lua_pushliteral(L, "isDocument"); // 9
	lua_pushliteral(L, "t");          // 10
	lua_pushliteral(L, "attrs");      // 11
	Keys k{7, 8, 9, 10, 11};
	Index *x = pushIndex(L, root, k); // 12: index, 13: top node
	if (!x) {
		s->noIndex = true;
		lua_pushnil(L);
		lua_pushnil(L);
		return 2;
	}
	const int idx = 12, topNode = 13;
	lua_getfenv(L, idx);
	lua_rawgeti(L, -1, X_NODES);
	lua_replace(L, -2); // 14: nodes
	const int nodes = 14;
	if (!x->complete) {
		if (!x->started) {
			// kept in the index's environment: compared by pointer, they must not be collected
			lua_getfenv(L, idx);
			lua_pushliteral(L, "#document");
			x->docTag = lua_tostring(L, -1);
			lua_rawseti(L, -2, X_NODES + 1);
			lua_pushliteral(L, "class");
			x->classKey = lua_tostring(L, -1);
			lua_rawseti(L, -2, X_NODES + 2);
			lua_pushliteral(L, "id");
			x->idKey = lua_tostring(L, -1);
			lua_rawseti(L, -2, X_NODES + 3);
			lua_pop(L, 1);
		}
		if (!buildIndex(L, x, nodes, topNode, k, budget)) {
			lua_pushboolean(L, 0);
			return 1;
		}
	}
	if (!s->started) {
		s->started = true;
		auto it = x->pos.find(lua_topointer(L, root));
		if (it == x->pos.end()) {
			s->noIndex = true;
			lua_pushnil(L);
			lua_pushnil(L);
			return 2;
		}
		s->root = it->second;
		s->next = s->root + 1;
		s->phase = 0;
	}
	Run r{L, x, s, nodes, testFn, testList};
	const Group &g = *s->group;
	const int end = x->end[s->root];
	while (true) {
		const Part *fpart = s->phase > 0 ? &g.parts[s->phase - 1] : nullptr;
		int flast = fpart ? (int)fpart->steps.size() - 1 : 0;
		while (s->next < end) {
			int i = s->next++;
			if (!fpart) {
				if (matchPlain(r, i, g, s->root)) {
					lua_rawgeti(L, nodes, i + 1);
					lua_rawseti(L, results, ++s->resultCount);
					if (s->firstOnly) return finish(L, s, results);
				}
			} else if (flast >= 0 && matchSteps(r, i, *fpart, flast, s->root)) {
				s->matched.push_back(i);
			}
			if (s->failed) {
				lua_pushnil(L);
				lua_pushstring(L, s->err.c_str());
				return 2;
			}
			if (--budget <= 0 && s->next < end) {
				lua_pushboolean(L, 0);
				return 1;
			}
		}
		if (fpart) {
			for (size_t m = 0; m < s->matched.size(); m++) {
				if (filterKeeps(*fpart, (int)m + 1)) {
					lua_rawgeti(L, nodes, s->matched[m] + 1);
					lua_rawseti(L, results, ++s->resultCount);
				}
			}
			s->matched.clear();
		}
		if (s->firstOnly && s->resultCount > 0) break; // the first result can't change any more
		size_t next = s->phase + 1;
		while (next <= g.parts.size() && g.parts[next - 1].filter == F_NONE) next++;
		if (next > g.parts.size()) break;
		s->phase = next;
		s->next = s->root + 1;
	}
	return finish(L, s, results);
}

int lua_selectorgc(lua_State *L)
{
	Selector *s = (Selector *)luaL_checkudata(L, 1, SELECTOR_META);
	s->~Selector();
	return 0;
}

// --- SHA-256 (FIPS 180-4), for the extension downloads checked against the repository index

const unsigned int SHA_K[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

inline unsigned int ror(unsigned int x, int n)
{
	return (x >> n) | (x << (32 - n));
}

void shaBlock(unsigned int H[8], const unsigned char *p)
{
	unsigned int w[64];
	for (int j = 0; j < 16; j++)
		w[j] = ((unsigned int)p[j * 4] << 24) | ((unsigned int)p[j * 4 + 1] << 16) | ((unsigned int)p[j * 4 + 2] << 8) | p[j * 4 + 3];
	for (int j = 16; j < 64; j++) {
		unsigned int s0 = ror(w[j - 15], 7) ^ ror(w[j - 15], 18) ^ (w[j - 15] >> 3);
		unsigned int s1 = ror(w[j - 2], 17) ^ ror(w[j - 2], 19) ^ (w[j - 2] >> 10);
		w[j] = w[j - 16] + s0 + w[j - 7] + s1;
	}
	unsigned int a = H[0], b = H[1], c = H[2], d = H[3], e = H[4], f = H[5], g = H[6], h = H[7];
	for (int j = 0; j < 64; j++) {
		unsigned int S1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25);
		unsigned int ch = (e & f) ^ (~e & g);
		unsigned int t1 = h + S1 + ch + SHA_K[j] + w[j];
		unsigned int S0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22);
		unsigned int maj = (a & b) ^ (a & c) ^ (b & c);
		unsigned int t2 = S0 + maj;
		h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
	}
	H[0] += a; H[1] += b; H[2] += c; H[3] += d; H[4] += e; H[5] += f; H[6] += g; H[7] += h;
}

// HashNative.sha256(bytes) -> lowercase hex digest
int lua_sha256(lua_State *L)
{
	size_t n = 0;
	const unsigned char *s = (const unsigned char *)luaL_checklstring(L, 1, &n);
	unsigned int H[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
	size_t full = n / 64 * 64;
	for (size_t i = 0; i < full; i += 64) shaBlock(H, s + i);
	unsigned char tail[128];
	size_t rest = n - full;
	memcpy(tail, s + full, rest);
	tail[rest] = 0x80;
	size_t tn = rest + 9 <= 64 ? 64 : 128;
	memset(tail + rest + 1, 0, tn - rest - 1);
	unsigned long long bits = (unsigned long long)n * 8;
	for (int i = 0; i < 8; i++) tail[tn - 1 - i] = (unsigned char)(bits >> (i * 8));
	for (size_t i = 0; i < tn; i += 64) shaBlock(H, tail + i);
	static const char *hex = "0123456789abcdef";
	char out[64];
	for (int i = 0; i < 8; i++)
		for (int j = 0; j < 8; j++) out[i * 8 + j] = hex[(H[i] >> (28 - j * 4)) & 15];
	lua_pushlstring(L, out, 64);
	return 1;
}

} // namespace

void luaSelect_init(lua_State *L)
{
	luaL_newmetatable(L, SELECTOR_META);
	lua_pushcfunction(L, lua_selectorgc);
	lua_setfield(L, -2, "__gc");
	lua_newtable(L);
	lua_pushcfunction(L, lua_selectorstep);
	lua_setfield(L, -2, "step");
	lua_setfield(L, -2, "__index");
	lua_pop(L, 1);
	luaL_newmetatable(L, INDEX_META);
	lua_pushcfunction(L, lua_indexgc);
	lua_setfield(L, -2, "__gc");
	lua_pop(L, 1);
	// HtmlNative.selector next to the parser (luaHtml_init runs first)
	lua_getglobal(L, "HtmlNative");
	if (lua_istable(L, -1)) {
		lua_pushcfunction(L, lua_newselector);
		lua_setfield(L, -2, "selector");
	}
	lua_pop(L, 1);
	lua_newtable(L);
	lua_pushcfunction(L, lua_sha256);
	lua_setfield(L, -2, "sha256");
	lua_setglobal(L, "HashNative");
}
