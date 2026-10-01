import time
from program_state import SimpleScriptState
from tuple_space import TupleSpace

def step(state: SimpleScriptState, thread_id: int | None = None, tuple_space: TupleSpace | None = None):
    """
    Execute the single instruction at state.pc, then advance the PC.
    On error, marks the state with set_error() instead of raising.
    thread_id is printed as a prefix by PRN (None = no prefix).
    """
    instr = state.current_instruction()
    if instr is None:
        state.finished = True
        return

    opcode, operands = instr

    try:
        if opcode == "SET":
            # SET @var value
            state.variables[operands[0][1:]] = state.resolve(operands[1])

        elif opcode in ("ADD", "SUB", "MUL", "DIV", "MOD"):
            # ADD $var val1 val2  (and SUB, MUL, DIV, MOD)
            a = state.resolve_int(operands[1])
            b = state.resolve_int(operands[2])

            if opcode == "ADD":
                result = a + b
            elif opcode == "SUB":
                result = a - b
            elif opcode == "MUL":
                result = a * b
            elif opcode == "DIV":
                if b == 0:
                    state.set_error(f"Division by zero at instruction {state.pc}")
                    return
                result = a // b
            elif opcode == "MOD":
                if b == 0:
                    state.set_error(f"Modulo by zero at instruction {state.pc}")
                    return
                result = a % b

            state.variables[operands[0][1:]] = result

        elif opcode in ("BGT", "BGE", "BLT", "BLE", "BEQ"):
            # BGT val1 val2 #label (and BGE, BLT, BLE, BEQ)
            a = state.resolve_int(operands[0])
            b = state.resolve_int(operands[1])
            label = operands[2]

            if label not in state.labels:
                state.set_error(f"Unknown label '{label}' at instruction {state.pc}")
                return

            jump = False
            if opcode == "BGT":
                jump = a > b
            elif opcode == "BGE":
                jump = a >= b
            elif opcode == "BLT":
                jump = a < b
            elif opcode == "BLE":
                jump = a <= b
            elif opcode == "BEQ":
                jump = a == b

            if jump:
                state.pc = state.labels[label]
                return  
            
        elif opcode == "BRA":
            # BRA #label
            label = operands[0]
            if label not in state.labels:
                state.set_error(f"Unknown label '{label}' at instruction {state.pc}")
                return
            state.pc = state.labels[label]
            return 

        elif opcode == "OUT":
            if tuple_space is None:
                state.set_error("OUT requires a tuple space")
                return
            values = [state.resolve(op) for op in operands]
            tuple_space.out(values)

            # Tell scheduler to check the registry and notify any peers
            # waiting for a pattern matching this tuple.
            state.last_out = tuple(values)

        elif opcode in ("RD", "IN"):
            if tuple_space is None:
                state.set_error(f"{opcode} requires a tuple space")
                return

            # None for undeclared vars (wildcards), concrete value otherwise.
            # Track which positions are wildcards so we can bind them on a match.
            pattern = []
            wildcard_vars = []
            for i, op in enumerate(operands):
                if op.startswith("@"):
                    pattern.append(None)
                    wildcard_vars.append((i, op[1:]))
                else:
                    pattern.append(state.resolve(op))

            matched = None

            # Priority 1: a peer already delivered a tuple (via NOTIFY or REGISTER response).
            # The scheduler sets pending_tuple when a background TCP thread gets a result.
            if state.pending_tuple is not None:
                matched = state.pending_tuple
                state.pending_tuple = None

            # Priority 2: check local tuple space.
            else:
                matched = (tuple_space.rd(pattern) if opcode == "RD"
                           else tuple_space.in_(pattern))

            if matched is not None:
                for i, varname in wildcard_vars:
                    state.variables[varname] = matched[i]

                # If we had registered with peers, tell the scheduler to cancel them.
                if state.registered:
                    state.block_pattern = pattern   # preserve for CANCEL message
                    state.needs_cancel  = True
                    state.registered    = False
            else:
                # No match found anywhere yet.
                # Store the block info so the scheduler can send REGISTER to peers.
                state.block_op      = opcode
                state.block_pattern = pattern
                return

        elif opcode == "SLP":
            # SLP val — suspend this thread for val seconds
            seconds = state.resolve_int(operands[0])
            state.sleep_until = time.time() + seconds

        elif opcode == "PRN":
            # PRN val1 val2 ... — resolve all values, print space-separated
            parts = [state.resolve_str(op) for op in operands]
            prefix = f"[Thread {thread_id}] " if thread_id is not None else ""
            print(f"{prefix}{' '.join(parts)}")

        elif opcode == "RET":
            state.finished = True
            return

        else:
            state.set_error(f"Unknown opcode '{opcode}' at instruction {state.pc}")
            return

    except RuntimeError as e:
        state.set_error(str(e))
        return

    state.pc += 1