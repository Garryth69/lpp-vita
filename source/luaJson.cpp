// Otakio: JsonNative, the JSON decoder of the app's json.lua in C. The Lua decoder spent ~75 us per value on the Vita
// (a 276 KB source listing: 67 steps of ~100 ms); this one builds the same tables a few dozen times faster.
//
// Same result as json.lua's Json.decode, value for value:
//   objects -> tables with string keys (a key whose value is null is left out; a repeated key keeps the last value)
//   arrays  -> tables 1..n (null elements are the `null` sentinel the caller passes, json.lua's Json.null)
//   numbers -> the same lenient grammar (-?digits[.digits][eE][-+]digits) read like Lua's tonumber (nil if invalid)
//   strings -> bytes as they are; escapes \" \\ \/ \b \f \n \r \t, \uXXXX with surrogate pairs (an invalid \u gives
//              "?", an unknown escape gives the character itself)
//   a top-level null -> nil; trailing NUL bytes are ignored; anything else after the value is an error
// Errors read "JSON: <what> at <1-based position>", like json.lua.
//
// JsonNative.new(text, null) -> decoder; decoder:step([values]) -> true, value when done; false while there is more;
// nil, error. Steps let a coroutine give the frame back between them. JsonNative.decode(text, null) does it in one go.
//
// The tables being built live on a Lua thread of their own (kept in the decoder's environment), so a step can stop
// anywhere and the next one carries on with the same stack.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <new>
#include <vector>
extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

namespace {

const char *DECODER_META = "Otakio.JsonDecoder";
const int E_TEXT = 1, E_NULL = 2, E_THREAD = 3; // decoder environment slots

enum Kind { ARRAY, OBJECT };
enum State { NEED_VALUE, CONTAINER_OPEN, AFTER_VALUE, VALUE_READY, DONE, FAILED };

struct Frame {
	Kind kind;
	int n; // array: elements so far
};

struct Decoder {
	const char *s = nullptr;
	size_t len = 0;
	size_t pos = 0; // 0-based
	State state = NEED_VALUE;
	std::vector<Frame> frames;
	std::vector<char> buf; // escaped strings
	char err[96] = {0};
	bool started = false;

	void fail(lua_State *T, const char *what, size_t at)
	{
		snprintf(err, sizeof(err), "JSON: %s at %u", what, (unsigned)(at + 1));
		state = FAILED;
		lua_settop(T, 0);
	}

	void skip()
	{
		while (pos < len) {
			char c = s[pos];
			if (c == ' ' || c == '\t' || c == '\r' || c == '\n') pos++;
			else break;
		}
	}

	static int hex4(const char *p, size_t avail, unsigned *out)
	{
		if (avail < 4) return 0;
		unsigned v = 0;
		for (int i = 0; i < 4; i++) {
			char c = p[i];
			v <<= 4;
			if (c >= '0' && c <= '9') v |= c - '0';
			else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
			else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
			else return 0;
		}
		*out = v;
		return 1;
	}

	void putUtf8(unsigned code)
	{
		if (code <= 0x7F) {
			buf.push_back((char)code);
		} else if (code <= 0x7FF) {
			buf.push_back((char)(0xC0 | (code >> 6)));
			buf.push_back((char)(0x80 | (code & 0x3F)));
		} else if (code <= 0xFFFF) {
			buf.push_back((char)(0xE0 | (code >> 12)));
			buf.push_back((char)(0x80 | ((code >> 6) & 0x3F)));
			buf.push_back((char)(0x80 | (code & 0x3F)));
		} else {
			buf.push_back((char)(0xF0 | (code >> 18)));
			buf.push_back((char)(0x80 | ((code >> 12) & 0x3F)));
			buf.push_back((char)(0x80 | ((code >> 6) & 0x3F)));
			buf.push_back((char)(0x80 | (code & 0x3F)));
		}
	}

	// s[pos] is the opening quote; pushes the string on T
	bool parseString(lua_State *T)
	{
		pos++;
		size_t start = pos;
		bool escaped = false;
		while (true) {
			size_t q = pos;
			while (q < len && s[q] != '"' && s[q] != '\\') q++;
			if (q >= len) {
				fail(T, "unterminated string", pos);
				return false;
			}
			if (s[q] == '"') {
				if (!escaped) {
					lua_pushlstring(T, s + start, q - start);
				} else {
					buf.insert(buf.end(), s + start, s + q);
					lua_pushlstring(T, buf.data(), buf.size());
				}
				pos = q + 1;
				return true;
			}
			if (!escaped) {
				escaped = true;
				buf.clear();
			}
			buf.insert(buf.end(), s + start, s + q);
			// json.lua reads the byte after the backslash even past the end (then the string is unterminated)
			unsigned char c = q + 1 < len ? (unsigned char)s[q + 1] : 0;
			if (c == 'u') {
				unsigned code;
				if (!hex4(s + q + 2, q + 2 <= len ? len - (q + 2) : 0, &code)) code = 63;
				size_t next = q + 6;
				if (code >= 0xD800 && code <= 0xDBFF && q + 7 < len && s[q + 6] == '\\' && s[q + 7] == 'u') {
					unsigned low;
					if (hex4(s + q + 8, q + 8 <= len ? len - (q + 8) : 0, &low) && low >= 0xDC00 && low <= 0xDFFF) {
						code = 0x10000 + (code - 0xD800) * 0x400 + (low - 0xDC00);
						next = q + 12;
					}
				}
				putUtf8(code);
				pos = next;
			} else {
				char out;
				switch (c) {
				case '"': out = '"'; break;
				case '\\': out = '\\'; break;
				case '/': out = '/'; break;
				case 'b': out = '\b'; break;
				case 'f': out = '\f'; break;
				case 'n': out = '\n'; break;
				case 'r': out = '\r'; break;
				case 't': out = '\t'; break;
				case 0: out = '?'; break; // backslash at the very end
				default: out = (char)c; break;
				}
				buf.push_back(out);
				pos = q + 2;
			}
			start = pos;
			if (pos > len) {
				fail(T, "unterminated string", pos);
				return false;
			}
		}
	}

	// json.lua: find(str, "^-?%d+%.?%d*[eE]?[-+]?%d*") then tonumber (nil when the match isn't a number)
	bool parseNumber(lua_State *T)
	{
		size_t p = pos;
		if (p < len && s[p] == '-') p++;
		size_t d = p;
		while (p < len && s[p] >= '0' && s[p] <= '9') p++;
		if (p == d) {
			fail(T, "bad number", pos);
			return false;
		}
		if (p < len && s[p] == '.') p++;
		while (p < len && s[p] >= '0' && s[p] <= '9') p++;
		if (p < len && (s[p] == 'e' || s[p] == 'E')) p++;
		if (p < len && (s[p] == '-' || s[p] == '+')) p++;
		while (p < len && s[p] >= '0' && s[p] <= '9') p++;
		char tmp[64];
		size_t n = p - pos;
		bool pushed = false;
		if (n < sizeof(tmp)) {
			memcpy(tmp, s + pos, n);
			tmp[n] = 0;
			char *end;
			double v = strtod(tmp, &end);
			if (end == tmp + n) {
				lua_pushnumber(T, v);
				pushed = true;
			}
		} else {
			// very long digit runs: go through Lua's own conversion
			lua_pushlstring(T, s + pos, n);
			if (lua_isnumber(T, -1)) {
				double v = lua_tonumber(T, -1);
				lua_pop(T, 1);
				lua_pushnumber(T, v);
				pushed = true;
			} else {
				lua_pop(T, 1);
			}
		}
		if (!pushed) lua_pushnil(T);
		pos = p;
		return true;
	}

	bool literal(const char *word, size_t n)
	{
		return pos + n <= len && memcmp(s + pos, word, n) == 0;
	}

	// One value (or the opening of a container) at pos
	bool parseValue(lua_State *T, int nullIdx)
	{
		skip();
		if (pos >= len) {
			fail(T, "unexpected end", pos);
			return false;
		}
		char c = s[pos];
		if (c == '{' || c == '[') {
			pos++;
			lua_newtable(T);
			frames.push_back(Frame{c == '{' ? OBJECT : ARRAY, 0});
			state = CONTAINER_OPEN;
			return true;
		}
		if (c == '"') {
			if (!parseString(T)) return false;
		} else if (c == 't' && literal("true", 4)) {
			pos += 4;
			lua_pushboolean(T, 1);
		} else if (c == 'f' && literal("false", 5)) {
			pos += 5;
			lua_pushboolean(T, 0);
		} else if (c == 'n' && literal("null", 4)) {
			pos += 4;
			lua_pushvalue(T, nullIdx);
		} else if (!parseNumber(T)) {
			return false;
		}
		state = VALUE_READY;
		return true;
	}

	// the key of the next member: s[pos] must be a quote
	bool parseKey(lua_State *T)
	{
		skip();
		if (pos >= len || s[pos] != '"') {
			fail(T, "expected key", pos);
			return false;
		}
		if (!parseString(T)) return false;
		skip();
		if (pos >= len || s[pos] != ':') {
			fail(T, "expected :", pos);
			return false;
		}
		pos++;
		state = NEED_VALUE;
		return true;
	}

	// T stack: [null sentinel] [container]... with, for an object being filled, its pending key on top of it
	bool step(lua_State *T, long budget)
	{
		const int nullIdx = 1;
		while (budget > 0) {
			lua_checkstack(T, 8);
			switch (state) {
			case NEED_VALUE:
				if (!parseValue(T, nullIdx)) return false;
				budget--;
				break;
			case CONTAINER_OPEN: {
				Frame &f = frames.back();
				skip();
				char close = f.kind == OBJECT ? '}' : ']';
				if (pos < len && s[pos] == close) {
					pos++;
					frames.pop_back();
					state = VALUE_READY;
				} else if (f.kind == OBJECT) {
					if (!parseKey(T)) return false;
				} else {
					state = NEED_VALUE;
				}
				break;
			}
			case VALUE_READY: {
				if (frames.empty()) {
					skip();
					if (pos < len) {
						fail(T, "unexpected trailing data", pos);
						return false;
					}
					state = DONE;
					return true;
				}
				Frame &f = frames.back();
				if (f.kind == ARRAY) {
					f.n++;
					lua_rawseti(T, -2, f.n);
				} else if (lua_rawequal(T, -1, nullIdx)) {
					lua_pop(T, 2); // a null member is left out, like json.lua
				} else {
					lua_rawset(T, -3);
				}
				state = AFTER_VALUE;
				break;
			}
			case AFTER_VALUE: {
				Frame &f = frames.back();
				skip();
				char c = pos < len ? s[pos] : 0;
				if (f.kind == ARRAY) {
					if (c == ']') {
						pos++;
						frames.pop_back();
						state = VALUE_READY;
					} else if (c == ',') {
						pos++;
						state = NEED_VALUE;
					} else {
						fail(T, "expected , or ]", pos);
						return false;
					}
				} else {
					if (c == '}') {
						pos++;
						frames.pop_back();
						state = VALUE_READY;
					} else if (c == ',') {
						pos++;
						if (!parseKey(T)) return false;
					} else {
						fail(T, "expected , or }", pos);
						return false;
					}
				}
				break;
			}
			case DONE:
			case FAILED:
				return true;
			}
		}
		return true;
	}
};

// JsonNative.new(text, null) -> decoder
int lua_newdecoder(lua_State *L)
{
	size_t n = 0;
	luaL_checklstring(L, 1, &n);
	lua_settop(L, 2);
	void *mem = lua_newuserdata(L, sizeof(Decoder));
	new (mem) Decoder();
	luaL_getmetatable(L, DECODER_META);
	lua_setmetatable(L, 3);
	lua_createtable(L, 3, 0);
	lua_pushvalue(L, 1);
	lua_rawseti(L, 4, E_TEXT);
	lua_pushvalue(L, 2);
	lua_rawseti(L, 4, E_NULL);
	lua_State *T = lua_newthread(L);
	lua_rawseti(L, 4, E_THREAD);
	lua_pushvalue(L, 2);
	lua_xmove(L, T, 1); // T[1]: the null sentinel
	lua_setfenv(L, 3);
	return 1;
}

// decoder:step([values]) -> true, value | false | nil, error
int lua_decoderstep(lua_State *L)
{
	Decoder *d = (Decoder *)luaL_checkudata(L, 1, DECODER_META);
	long budget = (long)luaL_optinteger(L, 2, 0x3FFFFFFF);
	if (budget < 1) budget = 1;
	if (d->state == FAILED) {
		lua_pushnil(L);
		lua_pushstring(L, d->err);
		return 2;
	}
	lua_getfenv(L, 1);
	lua_rawgeti(L, -1, E_TEXT);
	d->s = lua_tolstring(L, -1, &d->len);
	lua_pop(L, 1);
	lua_rawgeti(L, -1, E_THREAD);
	lua_State *T = lua_tothread(L, -1);
	lua_pop(L, 2);
	if (!d->started) {
		d->started = true;
		while (d->len > 0 && d->s[d->len - 1] == 0) d->len--; // a C terminator left at the end of a response
	}
	if (d->state != DONE) d->step(T, budget);
	if (d->state == FAILED) {
		lua_pushnil(L);
		lua_pushstring(L, d->err);
		return 2;
	}
	if (d->state != DONE) {
		lua_pushboolean(L, 0);
		return 1;
	}
	lua_pushboolean(L, 1);
	// the value is on top of T; a top-level null is nil
	if (lua_gettop(T) >= 2 && !lua_rawequal(T, -1, 1)) {
		lua_pushvalue(T, -1);
		lua_xmove(T, L, 1);
	} else {
		lua_pushnil(L);
	}
	return 2;
}

int lua_decodergc(lua_State *L)
{
	Decoder *d = (Decoder *)luaL_checkudata(L, 1, DECODER_META);
	d->~Decoder();
	return 0;
}

// JsonNative.decode(text, null) -> value | nil, error
int lua_decodejson(lua_State *L)
{
	lua_settop(L, 2);
	lua_pushcfunction(L, lua_newdecoder);
	lua_pushvalue(L, 1);
	lua_pushvalue(L, 2);
	lua_call(L, 2, 1);
	lua_pushcfunction(L, lua_decoderstep);
	lua_pushvalue(L, 3);
	lua_call(L, 1, 2);
	if (lua_isnil(L, -2)) return 2; // nil, error
	return 1;
}

const luaL_Reg JsonNative_functions[] = {
	{"new", lua_newdecoder},
	{"decode", lua_decodejson},
	{0, 0}
};

} // namespace

void luaJson_init(lua_State *L)
{
	luaL_newmetatable(L, DECODER_META);
	lua_pushcfunction(L, lua_decodergc);
	lua_setfield(L, -2, "__gc");
	lua_newtable(L);
	lua_pushcfunction(L, lua_decoderstep);
	lua_setfield(L, -2, "step");
	lua_setfield(L, -2, "__index");
	lua_pop(L, 1);
	lua_newtable(L);
	luaL_register(L, NULL, JsonNative_functions);
	lua_setglobal(L, "JsonNative");
}
