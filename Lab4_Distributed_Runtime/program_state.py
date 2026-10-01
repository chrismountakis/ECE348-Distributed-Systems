from dataclasses import dataclass, field
from typing import Any
import time

# Num of operands for each instr
# None means variable number of operands
INSTR_DEFS = {
    "SET": 2,       # SET $var value
    "ADD": 3,       # ADD $var val1 val2
    "SUB": 3,       
    "MUL": 3,
    "DIV": 3,
    "MOD": 3,
    "BGT": 3,       # BGT val1 val2 #label
    "BGE": 3,
    "BLT": 3,
    "BLE": 3,
    "BEQ": 3,
    "BRA": 1,       # BRA #label
    "OUT": None,    # OUT val1 val2 ... (at least 1 arg) — add tuple to tuple space
    "RD":  None,    # RD  val1 val2 ... (at least 1 arg) — read tuple (blocking)
    "IN":  None,    # IN  val1 val2 ... (at least 1 arg) — read & remove tuple (blocking)
    "SLP": 1,       # SLP val
    "PRN": None,    # PRN val1 val2 ... (0 or more)
    "RET": 0,
}

# Splits line into list of tokens (quoted strings are single tokens).
def _tokenize(line: str, line_num: int) -> list[str]:
    """
    Split an instruction line into tokens, respecting quoted strings.
 
    Example:
        'PRN "hello world" $x'  →  ['PRN', '"hello world"', '$x']
    """
    tokens: list[str] = []
    i = 0
    n = len(line)
 
    while i < n:
        # skip whitespace
        if line[i].isspace():
            i += 1
            continue
 
        # quoted string
        if line[i] == '"':
            j = i + 1
            while j < n and line[j] != '"':
                j += 1
            if j >= n:
                raise RuntimeError(
                    f"Line {line_num}: unterminated string literal"
                )
            tokens.append(line[i:j + 1])  
            i = j + 1
            continue
 
        # regular token (until next whitespace or quote)
        j = i
        while j < n and not line[j].isspace() and line[j] != '"':
            j += 1
        tokens.append(line[i:j])
        i = j
 
    return tokens
 
# Try to convert a string to int 
def _try_int(value: str) -> int | str:
    """Return an int if the string looks like one, otherwise the string."""
    try:
        return int(value)
    except ValueError:
        return value

@dataclass
class SimpleScriptState: 
    """
    Fields:
        name:       program name ($arg0),
        code:       list of parsed instructions, (opcode, [operands])
        labels:     dictionary mapping label name to pc 
        variables:  dictionary mapping var name to value (int or str)
        pc:         program counter - index into self.code
        finished:   True after a RET instruction or an error
        error:      if not None, holds the error message that caused termination
    """
    name: str                                           
    code: list = field(default_factory=list)            
    labels: dict = field(default_factory=dict)          
    variables: dict = field(default_factory=dict)      
    pc: int = 0                                         
    finished: bool = False                              
    error: str | None = None                           
    sleep_until: float | None = None

    registered:    bool       = False  # True after REGISTER sent to all peers
    pending_tuple: tuple|None = None   # tuple delivered by a peer, ready to consume
    block_op:      str|None   = None   # "RD" or "IN" — operation we're blocked on
    block_pattern: list|None  = None   # the pattern we're waiting for
    needs_cancel:  bool       = False  # True when scheduler must CANCEL peers
    last_out:      tuple|None = None   # set by OUT so scheduler notifies peers

    @classmethod
    def from_dict(cls, data: dict) -> "SimpleScriptState":                        
        return cls(                                           
            name          = data['name'],                                         
            code          = [(i[0], i[1]) for i in data['code']],
            labels        = data['labels'],                                       
            variables     = data['variables'],                
            pc            = data['pc'],                                           
            block_op      = data.get('block_op'),
            block_pattern = data.get('block_pattern'),                            
            pending_tuple = tuple(data['pending_tuple']) if   
            data.get('pending_tuple') else None,                   
            sleep_until   = time.time() + data['sleep_remaining'] if              
            data.get('sleep_remaining') else None,                          
            # registered = False: re-registers if needed
        )

    @classmethod
    def from_file(cls, filepath: str, args: list[str] | None = None) -> "SimpleScriptState":
        """
        Parses a SimpleScript source file and return a SimpleScriptState class.
 
        Args:
            filepath: path to the .ss source file
            args:     command-line arguments (excluding the program name)
 
        Raises:
            RuntimeError on syntax / parse errors.
        """
        if args is None:
            args = []
 
        with open(filepath, "r") as f:
            raw_lines = f.readlines()

        # First line must be the #SIMPLESCRIPT tag
        if not raw_lines or raw_lines[0].strip() != "#SIMPLESCRIPT":
            raise RuntimeError(
                "First line must be '#SIMPLESCRIPT'"
            )
 
        # Parse instruction lines 
        code: list[tuple[str, list[str]]] = []
        labels: dict[str, int] = {}
 
        for line_num, raw_line in enumerate(raw_lines[1:], start=2):
            stripped = raw_line.strip()
            if not stripped:
                continue 
 
            tokens = _tokenize(stripped, line_num)
            if not tokens:
                continue # safety check
 
            idx = 0  
 
            # Label at the start of the line 
            if tokens[0].startswith("#"):
                label = tokens[0]
                if label in labels:
                    raise RuntimeError(
                        f"Line {line_num}: duplicate label '{label}'"
                    )
                labels[label] = len(code)  # instr idx
                idx = 1
 
            # If line is only a label 
            if idx >= len(tokens):
                # points to the *next* instruction
                continue
 
            opcode = tokens[idx].upper()
            operands = tokens[idx + 1:]
 
            if opcode not in INSTR_DEFS:
                raise RuntimeError(
                    f"Line {line_num}: unknown instruction '{opcode}'"
                )
 
            expected = INSTR_DEFS[opcode]
            if expected is not None and len(operands) != expected:
                raise RuntimeError(
                    f"Line {line_num}: '{opcode}' expects {expected} "
                    f"operand(s), got {len(operands)}"
                )
            
            # for variable-arg instructions, check minimums
            if opcode in ("OUT", "RD", "IN") and len(operands) < 1:
                raise RuntimeError(
                    f"Line {line_num}: '{opcode}' needs at least 1 operand"
                )
 
            code.append((opcode, operands))
 
        # Set up args ($arg0, $arg1, ..., $argc)
        variables: dict[str, Any] = {}
        variables["arg0"] = filepath
        for i, arg in enumerate(args, start=1):
            variables[f"arg{i}"] = _try_int(arg)
        variables["argc"] = len(args) + 1
 
        return cls(
            name=filepath,
            code=code,
            labels=labels,
            variables=variables,
            pc=0,
        )
    
    # Helpers
    def resolve(self, token: str) -> int | str:
        """
        Resolve a token to its concrete value.
 
        - $varname:  look up in self.variables (error if not declared)
        - "string":  return the string (without quotes)
        - 123 / -42: return the integer
        """
        if token.startswith("$"):
            key = token[1:]
            if key not in self.variables:
                raise RuntimeError(
                    f"Undeclared variable '{token}' at instruction {self.pc}"
                )
            return self.variables[key]
 
        if token.startswith('"') and token.endswith('"'):
            return token[1:-1]
 
        # try integer
        try:
            return int(token)
        except ValueError:
            raise RuntimeError(
                f"Cannot resolve token '{token}' at instruction {self.pc}"
            )
 
    def resolve_int(self, token: str) -> int:
        """Resolve a token and ensure the result is an integer."""
        val = self.resolve(token)
        if not isinstance(val, int):
            raise RuntimeError(
                f"Expected integer, got string '{val}' at instruction {self.pc}"
            )
        return val
 
    def resolve_str(self, token: str) -> str:
        """Resolve a token and return it as a string (ints are converted)."""
        val = self.resolve(token)
        return str(val)
 
    # Utils
    def current_instruction(self) -> tuple[str, list[str]] | None:
        """Return the instruction at the current PC, or None if finished."""
        if self.finished or self.pc < 0 or self.pc >= len(self.code):
            return None
        return self.code[self.pc]
 
    def set_error(self, msg: str):
        """Mark the program as terminated with an error."""
        self.error = msg
        self.finished = True
 
    def __repr__(self) -> str:
        status = "ERROR" if self.error else ("DONE" if self.finished else "RUNNING")
        return (
            f"ProgramState(name='{self.name}', pc={self.pc}, "
            f"status={status}, vars={len(self.variables)})"
        )