#include "debug.h"

#ifdef _DEBUG

#include "../Games8Th/utils/console/console.h"

#include <Windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>

// AD log rides Con - one pipeline (console + file + ODS).
// Rate-limited so weather/spawn spam cannot stall inject / Present.
void initDebug()
{
	ADLog("BOOT", "debug:init", "ready", nullptr);
}

void ADLog(const char* hyp, const char* loc, const char* msg, const char* dataJson)
{
	const char* h = (hyp && hyp[0]) ? hyp : "?";
	const char* m = (msg && msg[0]) ? msg : "?";
	const char* l = (loc && loc[0]) ? loc : "";

	char rkey[96];
	_snprintf_s(rkey, sizeof(rkey), _TRUNCATE, "ad:%s:%s", h, m);

	char body[768];
	if (dataJson && dataJson[0]) {
		if (l[0])
			_snprintf_s(body, sizeof(body), _TRUNCATE, "%s  %s  %s  %s", h, m, l, dataJson);
		else
			_snprintf_s(body, sizeof(body), _TRUNCATE, "%s  %s  %s", h, m, dataJson);
	} else if (l[0]) {
		_snprintf_s(body, sizeof(body), _TRUNCATE, "%s  %s  %s", h, m, l);
	} else {
		_snprintf_s(body, sizeof(body), _TRUNCATE, "%s  %s", h, m);
	}

	if (h[0] == 'B' && h[1] == 'O') {
		Con::
Tag("ad", Con::
Level::
Ok, "%s", body);
		return;
	}
	// Visible at default min-level; 500ms rate per hyp+msg
	Con::
RateAt(rkey, 500, Con::
Level::
Info, "%s", body);
}

void ADLogf(const char* hyp, const char* loc, const char* msg, const char* fmt, ...)
{
	if (!fmt || !fmt[0]) {
		ADLog(hyp, loc, msg, nullptr);
		return;
	}
	char data[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf_s(data, sizeof(data), _TRUNCATE, fmt, ap);
	va_end(ap);
	ADLog(hyp, loc, msg, data);
}

#endif

