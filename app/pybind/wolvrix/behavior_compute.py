"""Lower authenticated stateless behavior carriers to kBehaviorComputeGroup."""
import copy
import hashlib
import re


def _attr(value):
    if isinstance(value, bool):
        kind = "bool"
    elif isinstance(value, int):
        kind = "int"
    elif isinstance(value, str):
        kind = "string"
    elif isinstance(value, list):
        if not value:
            kind = "string[]"
        elif all(isinstance(x, bool) for x in value):
            kind = "bool[]"
        elif all(isinstance(x, int) for x in value):
            kind = "int[]"
        else:
            kind = "string[]"
    else:
        raise ValueError("unsupported behavior compute attribute")
    return {"t": kind, "vs": value} if isinstance(value, list) else {"t": kind, "v": value}


def _ports(graph):
    vals = {v["sym"]: v for v in graph["vals"]}
    result = []
    for key, direction in (("in", "input"), ("out", "output")):
        for port in graph["ports"].get(key, []):
            value = vals.get(port["val"])
            if value is None:
                raise ValueError("compute carrier port value missing: " + port["name"])
            result.append({"name": port["name"], "direction": direction, "width": value["w"], "val": port["val"]})
    return result


def _binding_key(model):
    binding = model.get("compute_binding")
    if not binding or binding.get("version") != 1 or binding.get("kind") != "compute":
        raise ValueError("missing/unsupported compute binding")
    module = binding.get("module")
    model_name = binding.get("model")
    if not isinstance(module, str) or not module.startswith("WolvrixBehaviorComputeGroup_v1_"):
        raise ValueError("unknown compute carrier module")
    supported = {
        "int-mul", "fp-mul", "vec-int-mul", "vec-fp-mul",
        "int-alu", "int-bku",
        "vec-int-alu-lane", "vec-int-alu-whole",
        "vec-int-pu-lane", "vec-int-pu-whole",
        "vec-int-mac-lane", "vec-int-mac-whole",
    }
    if model_name not in supported:
        raise ValueError("unknown compute behavior model")
    boundary = binding.get("boundary", "none")
    if boundary not in {"none", "lane-core", "whole-unit"}:
        raise ValueError("unknown compute behavior boundary: " + str(boundary))
    if model_name.endswith("-lane") and boundary != "lane-core":
        raise ValueError("lane model requires lane-core boundary: " + model_name)
    if model_name.endswith("-whole") and boundary != "whole-unit":
        raise ValueError("whole model requires whole-unit boundary: " + model_name)
    operations = binding.get("operations", [])
    if not isinstance(operations, list) or not all(isinstance(op, str) and op for op in operations):
        raise ValueError("compute binding has invalid operation list")
    manifest_ops = model.get("compute_ops", [])
    if manifest_ops:
        manifest_ids = [op.get("id") for op in manifest_ops]
        if manifest_ids != operations:
            raise ValueError("compute binding operation list does not match manifest")
    ports = binding.get("ports")
    if not isinstance(ports, list) or not ports:
        raise ValueError("compute binding has no ports")
    operation_spec = ";".join(operations)
    suffix = "" if boundary == "none" and not operation_spec else "|" + boundary + "|" + operation_spec
    canonical = "behavior-compute-group-v1|" + model_name + "|" + module + "|" + "|".join(
        f"{p['name']}:{p['direction']}:{p['width']}" for p in ports
    ) + suffix
    digest = hashlib.sha256(canonical.encode()).hexdigest()
    if binding.get("semantic_sha256") != digest:
        raise ValueError("compute binding semantic hash mismatch: " + module)
    return module, binding


def lower_behavior_compute(design, manifest, source_dir=None):
    """Replace carrier module graphs with pure compute operations.

    The carrier's generated Chisel body remains a legal SV fallback. This pass
    only replaces an exact, typed module boundary after the manifest has
    authenticated its model ID and semantic port description.
    """
    if manifest.get("version") not in (3,):
        raise ValueError("compute-grouped lowering requires binding manifest v3")
    bindings = {}
    for model in manifest.get("models", []):
        if model.get("lowering") != "compute-grouped":
            if model.get("compute_binding") not in (None, "null"):
                raise ValueError("ordinary model has compute binding")
            continue
        module, binding = _binding_key(model)
        if module in bindings and bindings[module]["model"] != binding["model"]:
            raise ValueError("duplicate compute carrier with conflicting models")
        bindings[module] = binding

    # Chisel may elaborate several instances of the same RawModule carrier.
    # FIRRTL/firtool commonly emits one canonical module and gives duplicate
    # definitions an ``_1``/``_2`` suffix before later deduplicating them from
    # the split Verilog file list.  Treat those names as aliases only when the
    # authenticated model and port signature are identical.  This keeps the
    # binding check strict while accepting the normal multi-instance case.
    def root_module(name):
        return re.sub(r"_\d+$", "", name)

    canonical = {}
    for module, binding in bindings.items():
        root = root_module(module)
        previous = canonical.get(root)
        if previous is not None:
            if (previous["model"], previous.get("ports")) != (binding["model"], binding.get("ports")):
                raise ValueError("compute carriers with conflicting duplicate module names: " + root)
        else:
            canonical[root] = binding

    result = copy.deepcopy(design)
    found = set()
    for graph in result.get("graphs", []):
        name = graph.get("symbol")
        binding = bindings.get(name)
        if binding is None:
            binding = canonical.get(root_module(name))
        if binding is None:
            continue
        found.add(name)
        actual = _ports(graph)
        expected = [dict(name=p["name"], direction=p["direction"], width=p["width"]) for p in binding["ports"]]
        actual_shape = [dict(name=p["name"], direction=p["direction"], width=p["width"]) for p in actual]
        if actual_shape != expected or graph.get("ports", {}).get("inout"):
            raise ValueError("compute carrier port binding mismatch: " + name)
        inputs = [p["val"] for p in actual if p["direction"] == "input"]
        outputs = [p["val"] for p in actual if p["direction"] == "output"]
        if not inputs or not outputs:
            raise ValueError("compute carrier must have input and output")
        graph["ops"] = [
            {
                "sym": "behavior_compute_group_" + name,
                "kind": "kBehaviorComputeGroup",
                "in": inputs,
                "out": outputs,
                "attrs": {
                    "behavior.version": _attr(1),
                    "behavior.model": _attr(binding["model"]),
                    "behavior.boundary": _attr(binding.get("boundary", "none")),
                    "behavior.operations": _attr(binding.get("operations", [])),
                    "behavior.observerOnly": _attr(False),
                    "behavior.stateful": _attr(False),
                    "behavior.inputWidths": _attr([p["width"] for p in actual if p["direction"] == "input"]),
                    "behavior.outputWidths": _attr([p["width"] for p in actual if p["direction"] == "output"]),
                    "behavior.bindingHash": _attr(binding["semantic_sha256"]),
                },
            }
        ]
        graph["declaredSymbols"] = []
    found_roots = {root_module(name) for name in found}
    missing = [name for name in bindings if root_module(name) not in found_roots]
    if missing:
        raise ValueError("unbound compute carriers: " + repr(sorted(missing)))
    if not found:
        raise ValueError("compute-grouped lowering requested without any bound carrier")
    return result
