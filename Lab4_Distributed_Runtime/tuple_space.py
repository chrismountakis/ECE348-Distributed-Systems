import threading

class TupleSpace:
    def __init__(self):
        self._tuples: list[tuple] = []
        self._lock = threading.Lock()   # scheduler + TCP server access this

    def match(self, stored: tuple, pattern: list) -> bool:
        """True if stored tuple matches pattern — None elements are wildcards."""
        if len(stored) != len(pattern):
            return False
        return all(p is None or s == p for s, p in zip(stored, pattern))

    def out(self, values: list) -> None:
        with self._lock:
            self._tuples.append(tuple(values))

    def rd(self, pattern: list) -> tuple | None:
        """Return first matching tuple without removing it."""
        with self._lock:
            for t in self._tuples:
                if self.match(t, pattern):
                    return t
            return None

    def in_(self, pattern: list) -> tuple | None:
        """Remove and return first matching tuple."""
        with self._lock:
            for i, t in enumerate(self._tuples):
                if self.match(t, pattern):
                    del self._tuples[i]
                    return t
            return None
        
    def ret_tuples(self) -> list[tuple]:
      with self._lock:
          tuples = list(self._tuples)
          self._tuples.clear()
          return tuples

    def __repr__(self) -> str:
        return f"TupleSpace({self._tuples})"