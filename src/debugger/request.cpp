#include <debugger/impl.h>
#include <debugger/protocol.h>
#include <debugger/path.h>
#include <debugger/luathread.h>
#include <debugger/io/base.h>
#include <bee/utility/unicode_win.h>

namespace vscode
{
	void closeprocess();

	void debugger_impl::close()
	{
		if (is_state(eState::terminated) || is_state(eState::birth)) {
            network_->close();
			return;
		}
		update_redirect();
        stdout_vtmode(stdout_buf_);
        output_raw("stdout", stdout_buf_, stdout_src_, stdout_line_);
        stdout_vt_.clear();
        stdout_buf_.clear();
        stdout_src_ = 0;
        stdout_line_ = 0;
		if (!attach_) {
			close_redirect();
		}
		set_state(eState::terminated);
		event_terminated();

		if (!attach_) {
			detach_all(false);
		}

		breakpointmgr_.clear();
		seq = 1;
        network_->close();
	}

	void debugger_impl::set_state(eState state)
	{
		if (state_ == state) return;
		state_ = state;
	}

	void debugger_impl::set_stepping(const char* reason)
	{
		set_state(eState::stepping);
		stopReason_ = reason;
	}

	bool debugger_impl::is_state(eState state) const
	{
		return state_ == state;
	}

	bool debugger_impl::request_initialize(rprotocol& req) {
		if (!is_state(eState::birth)) {
			response_error(req, "already initialized");
			return false;
		}
		auto& args = req["arguments"];
		if (args.HasMember("locale") && args["locale"].IsString()) {
			setlang(args["locale"].Get <std::string>());
		}
		response_initialize(req);
		set_state(eState::initialized);
		event_initialized();
		event_capabilities();
		return false;
	}

	bool debugger_impl::request_launch(rprotocol& req)
	{
#if defined(DEBUGGER_ENABLE_LAUNCH)
		attach_ = false;
		return request_launch_or_attach(req);
#else
		return request_attach(req);
#endif
	}

	bool debugger_impl::request_attach(rprotocol& req)
	{
		attach_ = true;
		return request_launch_or_attach(req);
	}

	bool debugger_impl::request_launch_or_attach(rprotocol& req)
	{
		initproto_ = rprotocol();
		if (!is_state(eState::initialized)) {
			response_error(req, "not initialized or unexpected state");
			return false;
		}
		config_.init(1, req["arguments"]);
		initialize_pathconvert(config_);
		auto consoleCoding = config_.get("consoleCoding", rapidjson::kStringType).Get<std::string>();
		if (consoleCoding == "ansi") {
			consoleSourceCoding_ = eCoding::ansi;
		}
		else if(consoleCoding == "utf8") {
			consoleSourceCoding_ = eCoding::utf8;
		}
		auto& sourceCoding = config_.get("sourceCoding", rapidjson::kStringType);
		if (sourceCoding.Get<std::string>() == "utf8") {
			sourceCoding_ = eCoding::utf8;
		}
		else if (sourceCoding.Get<std::string>() == "ansi") {
			sourceCoding_ = eCoding::ansi;
		}

		nodebug_ = config_.get("noDebug", rapidjson::kFalseType).GetBool();
		response_success(req);
		initproto_ = std::move(req);
		return false;
	}

	bool debugger_impl::request_configuration_done(rprotocol& req_) {
		response_success(req_);
		if (initproto_.IsNull()) {
			return false;
		}
		rprotocol& req = initproto_;
		bool stopOnEntry = true;
		auto& args = req["arguments"];
		if (args.HasMember("stopOnEntry") && args["stopOnEntry"].IsBool()) {
			stopOnEntry = args["stopOnEntry"].GetBool();
		}

		if (stopOnEntry) {
			set_stepping("entry");
		}
		else {
			set_state(eState::running);
		}
		if (on_clientattach_) {
			on_clientattach_();
		}
		return !stopOnEntry;
	}

	bool debugger_impl::request_threads(rprotocol& req) {
		response_threads(req);
		return false;
	}

	bool debugger_impl::request_stack_trace(rprotocol& req, debug& debug) {
		lua_State* L = debug.L();
		auto& args = req["arguments"];
		if (!args.HasMember("threadId") || !args["threadId"].IsInt()) {
			response_error(req, "Not found thread");
			return false;
		}
		int threadId = args["threadId"].GetInt();
		luathread* thread = find_luathread(threadId);
		if (!thread) {
			response_error(req, "Not found thread");
			return false;
		}

		int depth = 0;
		int levels = args.HasMember("levels") ? args["levels"].GetInt() : 200;
		levels = (levels != 0 ? levels : 200);
		int startFrame = args.HasMember("startFrame") ? args["startFrame"].GetInt() : 0;
		int endFrame = startFrame + levels;
		int curFrame = 0;
		int virtualFrame = 0;
		
		response_success(req, [&](wprotocol& res)
		{
			lua::Debug entry;
			for (auto _ : res("stackFrames").Array())
			{
				if (startFrame == 0 && debug.is_virtual()) {
					for (auto _ : res.Object()) {
						source* s = openVSource();
						if (s && s->valid) {
							s->output(res);
							res("id").Int(threadId << 16 | 0xFFFF);
							res("name").String("");  // TODO
							res("line").Int(debug.currentline());
							res("column").Int(1);
						}
					}
					virtualFrame++;
				}

				while (lua_getstack(L, depth, (lua_Debug*)&entry))
				{
					if (curFrame != 0 && (curFrame < startFrame || curFrame >= endFrame)) {
						depth++;
						curFrame++;
						continue;
					}
					int status = lua_getinfo(L, "Sln", (lua_Debug*)&entry);
					assert(status);
					if (curFrame == 0) {
						if (*entry.what == 'C') {
							depth++;
							continue;
						}
						else {
							source* s = sourcemgr_.create(&entry);
							if (!s || !s->valid) {
								depth++;
								continue;
							}
						}
					}
					if (curFrame < startFrame || curFrame >= endFrame) {
						depth++;
						curFrame++;
						continue;
					}
					curFrame++;

					if (*entry.what == 'C') {
						for (auto _ : res.Object()) {
							res("id").Int(threadId << 16 | depth);
							res("presentationHint").String("label");
							res("name").String(*entry.what == 'm' ? "[main chunk]" : (entry.name ? entry.name : "?"));
							res("line").Int(0);
							res("column").Int(0);
						}
					}
					else {
						for (auto _ : res.Object()) {
							source* s = sourcemgr_.create(&entry);
							if (!s) {
								// TODO?
							}
							else if (s->valid) {
								s->output(res);
							}
							else {
								res("presentationHint").String("label");
							}
							res("id").Int(threadId << 16 | depth);
							res("name").String(*entry.what == 'm' ? "[main chunk]" : (entry.name ? entry.name : "?"));
							res("line").Int(entry.currentline);
							res("column").Int(1);
						}
					}
					depth++;
				}
			}
			res("totalFrames").Int(curFrame + virtualFrame);
		});
		return false;
	}

	bool debugger_impl::request_source(rprotocol& req, debug& debug) {
		lua_State* L = debug.L();
		auto& args = req["arguments"];
		std::string code;
		if (sourcemgr_.getCode(args["sourceReference"].GetUint(), code)) {
			response_source(req, code);
		}
		else {
			response_source(req, "Source not available");
		}
		return false;
	}

	bool debugger_impl::request_set_breakpoints(rprotocol& req)
	{
		auto& args = req["arguments"];
		vscode::source* s = sourcemgr_.create(args["source"]);
		if (!s || !s->valid) {
			response_error(req, "not yet implemented");
			return false;
		}
		for (auto& lt : luathreads_) {
			lt.second->update_breakpoint();
		}
		response_success(req, [&](wprotocol& res)
		{
			breakpointmgr_.set_breakpoint(*s, args, res);
		});
		return false;
	}

	bool debugger_impl::request_set_exception_breakpoints(rprotocol& req)
	{
		auto& args = req["arguments"];
		exception_.clear();
		if (args.HasMember("filters") && args["filters"].IsArray()) {
			for (auto& v : args["filters"].GetArray()) {
				if (v.IsString()) {
					std::string filter = v.Get<std::string>();
					if (filter == "all") {
						exception_.insert(eException::lua_panic);
						exception_.insert(eException::lua_pcall);
						exception_.insert(eException::xpcall);
						exception_.insert(eException::pcall);
					}
					else if (filter == "lua_panic") {
						exception_.insert(eException::lua_panic);
					}
					else if (filter == "lua_pcall") {
						exception_.insert(eException::lua_pcall);
					}
					else if (filter == "pcall") {
						exception_.insert(eException::pcall);
					}
					else if (filter == "xpcall") {
						exception_.insert(eException::xpcall);
					}
				}
			}

		}
		response_success(req);
		return false;
	}

	bool debugger_impl::request_scopes(rprotocol& req, debug& debug) {
		auto& args = req["arguments"];
		if (!args.HasMember("frameId")) {
			response_error(req, "Not found frame");
			return false;
		}
		int threadAndFrameId = args["frameId"].GetInt();
		int threadId = threadAndFrameId >> 16;
		int frameId = threadAndFrameId & 0xFFFF;
		luathread* thread = find_luathread(threadId);
		if (!thread) {
			response_error(req, "Not found thread");
			return false;
		}
		thread->new_frame(debug, *this, req, frameId);
		return false;
	}

	bool debugger_impl::request_variables(rprotocol& req, debug& debug) {
		auto& args = req["arguments"];
		int64_t valueId = args["variablesReference"].GetInt64();
		int threadId = valueId >> 32;
		int frameId = (valueId >> 16) & 0xFFFF;
		luathread* thread = find_luathread(threadId);
		if (!thread) {
			response_error(req, "Not found thread");
			return false;
		}
		thread->get_variable(debug, *this, req, valueId, frameId);
		return false;
	}

	bool debugger_impl::request_set_variable(rprotocol& req, debug& debug) {
		auto& args = req["arguments"];
		int64_t valueId = args["variablesReference"].GetInt64();
		int threadId = valueId >> 32;
		int frameId = (valueId >> 16) & 0xFFFF;
		luathread* thread = find_luathread(threadId);
		if (!thread) {
			response_error(req, "Not found thread");
			return false;
		}
		thread->set_variable(debug, *this, req, valueId, frameId);
		return false;
	}

	bool debugger_impl::request_terminate(rprotocol& req)
	{
		response_success(req);
        close();
		closeprocess();
		return true;
	}

	bool debugger_impl::request_disconnect(rprotocol& req)
	{
		response_success(req);
		on_disconnect();
		return true;
	}

	bool debugger_impl::request_stepin(rprotocol& req, debug& debug)
	{
		auto& args = req["arguments"];
		if (!args.HasMember("threadId") || !args["threadId"].IsInt()) {
			response_error(req, "Not found thread");
			return false;
		}
		int threadId = args["threadId"].GetInt();
		luathread* thread = find_luathread(threadId);
		if (!thread) {
			response_error(req, "Not found thread");
			return false;
		}
		set_stepping("step");
		thread->step_in();
		response_success(req);
		return true;
	}

	bool debugger_impl::request_stepout(rprotocol& req, debug& debug)
	{
		lua_State* L = debug.L();
		auto& args = req["arguments"];
		if (!args.HasMember("threadId") || !args["threadId"].IsInt()) {
			response_error(req, "Not found thread");
			return false;
		}
		int threadId = args["threadId"].GetInt();
		luathread* thread = find_luathread(threadId);
		if (!thread) {
			response_error(req, "Not found thread");
			return false;
		}
		set_stepping("step");
		thread->step_out(L);
		response_success(req);
		return true;
	}

	bool debugger_impl::request_next(rprotocol& req, debug& debug)
	{
		lua_State* L = debug.L();
		auto& args = req["arguments"];
		if (!args.HasMember("threadId") || !args["threadId"].IsInt()) {
			response_error(req, "Not found thread");
			return false;
		}
		int threadId = args["threadId"].GetInt();
		luathread* thread = find_luathread(threadId);
		if (!thread) {
			response_error(req, "Not found thread");
			return false;
		}
		set_stepping("step");
		thread->step_over(L);
		response_success(req);
		return true;
	}

	bool debugger_impl::request_continue(rprotocol& req, debug& debug)
	{
		response_success(req);
		set_state(eState::running);
		return true;
	}

	bool debugger_impl::request_pause(rprotocol& req)
	{
		auto& args = req["arguments"];
		if (!args.HasMember("threadId") || !args["threadId"].IsInt()) {
			response_error(req, "Not found thread");
			return false;
		}
		int threadId = args["threadId"].GetInt();
		luathread* thread = find_luathread(threadId);
		if (!thread) {
			response_error(req, "Not found thread");
			return false;
		}
		set_stepping("pause");
		thread->step_in();
		response_success(req);
		return true;
	}

	bool debugger_impl::request_evaluate(rprotocol& req, debug& debug)
	{
		lua_State* L = debug.L();
		auto& args = req["arguments"];
		if (!args.HasMember("frameId")) {
			response_error(req, "Not yet implemented.");
			return false;
		}
		int threadAndFrameId = args["frameId"].GetInt();
		int threadId = threadAndFrameId >> 16;
		int frameId = threadAndFrameId & 0xFFFF;
		luathread* thread = find_luathread(threadId);
		if (!thread) {
			response_error(req, "Not found thread");
			return false;
		}
		lua::Debug current;
		if (!lua_getstack(L, frameId, (lua_Debug*)&current)) {
			response_error(req, "Error frame");
			return false;
		}
		thread->evaluate(L, &current, *this, req, frameId);
		return false;
	}

	std::string safe_tostr(lua_State* L, int idx)
	{
		if (lua_type(L, idx) == LUA_TSTRING) {
			size_t len = 0;
			const char* str = lua_tolstring(L, idx, &len);
			return std::string(str, len);
		}
		return std::string(lua_typename(L, lua_type(L, idx)));
	}

	bool debugger_impl::request_exception_info(rprotocol& req, debug& debug)
	{
		lua_State* L = debug.L();
		//TODO: threadId
		response_success(req, [&](wprotocol& res)
		{
			res("breakMode").String("always");
			std::string exceptionId = safe_tostr(L, -2);
			luaL_traceback(L, L, 0, (int)lua_tointeger(L, -1));
			std::string stackTrace = safe_tostr(L, -1);
			lua_pop(L, 1);

			exceptionId = path_exception(exceptionId);
			stackTrace = path_exception(stackTrace);

			res("exceptionId").String(exceptionId);
			for (auto _ : res("details").Object())
			{
				res("stackTrace").String(stackTrace);
			}
		});
		return false;
	}

	bool debugger_impl::request_loaded_sources(rprotocol& req, debug& debug)
	{
		response_success(req, [&](wprotocol& res)
		{
			sourcemgr_.loadedSources(res);
		});
		return false;
	}
}
