#include "emmy_debugger/proto/proto_handler.h"

#include "emmy_debugger/emmy_facade.h"
#include "emmy_debugger/debugger/emmy_debugger.h"
#include "emmy_debugger/transporter/transporter.h"
#include "nlohmann/json.hpp"

ProtoHandler::ProtoHandler(EmmyFacade *owner)
	: _owner(owner) {
}

void ProtoHandler::OnDispatch(nlohmann::json document) {
	if (document["cmd"].is_number_integer()) {
		const MessageCMD command = document["cmd"].get<MessageCMD>();
		if (_owner->IsAuthenticationRequired() && !_owner->IsAuthenticated() &&
			command != MessageCMD::InitReq && command != MessageCMD::EnvelopeV2) {
			return;
		}
		switch (command) {
			case MessageCMD::EnvelopeV2: {
				_owner->OnV2Envelope(document);
				break;
			}
			case MessageCMD::InitReq: {
				InitParams params;
				params.Deserialize(document);
				if (_owner->AuthenticateInit(params.authToken)) {
					OnInitReq(params);
				}
				break;
			}
			case MessageCMD::ReadyReq: {
				if (_owner->IsAuthenticationRequired() && !_owner->IsAuthenticated()) {
					break;
				}
				OnReadyReq();
				break;
			}
			case MessageCMD::AddBreakPointReq: {
				AddBreakpointParams params;
				params.Deserialize(document);
				OnAddBreakPointReq(params);
				break;
			}
			case MessageCMD::RemoveBreakPointReq: {
				RemoveBreakpointParams params;
				params.Deserialize(document);
				OnRemoveBreakPointReq(params);
				break;
			}
			case MessageCMD::ActionReq: {
				ActionParams params;
				params.Deserialize(document);
				OnActionReq(params);
				break;
			}
			case MessageCMD::EvalReq: {
				EvalParams params;
				params.Deserialize(document);
				OnEvalReq(params);
				break;
			}
			default:
				break;
		}
	}
}

void ProtoHandler::OnInitReq(InitParams &params) {
	_owner->InitReq(params);
}

void ProtoHandler::OnReadyReq() {
	_owner->ReadyReq();
}

void ProtoHandler::OnAddBreakPointReq(AddBreakpointParams &params) {
	auto &manager = _owner->GetDebugManager();
	if (params.clear) {
		manager.RemoveAllBreakpoints();
	}
	if (params.replaceComposite) {
		manager.ReplaceBreakpoints(params.breakPoints);
	} else {
		for (auto &bp: params.breakPoints) {
			manager.AddBreakpoint(bp);
		}
	}
}

void ProtoHandler::OnRemoveBreakPointReq(RemoveBreakpointParams &params) {
	auto &manager = _owner->GetDebugManager();
	for (auto bp: params.breakPoints) {
		if (bp) {
			manager.RemoveBreakpoint(bp->file, bp->line, bp->owner, bp->breakpointId, bp->vmId);
		}
	}
}

void ProtoHandler::OnActionReq(ActionParams &params) {
	auto &manager = _owner->GetDebugManager();
	if (params.vmId != 0) {
		manager.RouteAction(params.vmId, params.action, params.pauseId, params.threadId);
	} else {
		manager.DoAction(params.action);
	}
}

void ProtoHandler::OnEvalReq(EvalParams &params) {
	auto &manager = _owner->GetDebugManager();
	if (params.ctx && params.ctx->vmId != 0) {
		const auto route = manager.RouteEval(params.ctx->vmId, params.ctx);
		if (!route.ok) {
			params.ctx->success = false;
			params.ctx->error = route.errorCode == nullptr ? "EVAL_REJECTED" : route.errorCode;
			_owner->OnEvalResult(params.ctx);
		}
	} else {
		const auto debuggers = manager.GetDebuggers();
		if (params.ctx && (debuggers.size() != 1 || !debuggers.front()->Eval(params.ctx))) {
			params.ctx->success = false;
			params.ctx->error = debuggers.size() > 1 ? "AMBIGUOUS_VM" : "VM_NOT_READY";
			_owner->OnEvalResult(params.ctx);
		}
	}
}
