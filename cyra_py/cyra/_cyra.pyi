from collections.abc import Sequence
import datetime
import enum
from typing import overload


class Binding:
    @property
    def node(self) -> str: ...

    @property
    def group(self) -> str: ...

    @property
    def name(self) -> str: ...

    @property
    def pid(self) -> int: ...

    @property
    def tid(self) -> int: ...

class Children(enum.Enum):
    Reset = 0

    Inherit = 1

class Finding:
    @property
    def id(self) -> str: ...

    @property
    def verdict(self) -> Verdict: ...

    @property
    def detail(self) -> str: ...

class Kind(enum.Enum):
    Worker = 0

    Release = 1

class Match:
    @property
    def irq(self) -> str: ...

    @property
    def process(self) -> str: ...

    @property
    def thread(self) -> str: ...

    def empty(self) -> bool: ...

class Options:
    def __init__(self, policy: Policy = Policy.Inherit, priority: int = 0, cpus: Sequence[int] = [], children: Children = Children.Reset) -> None: ...

    @property
    def policy(self) -> Policy: ...

    @policy.setter
    def policy(self, arg: Policy, /) -> None: ...

    @property
    def priority(self) -> int: ...

    @priority.setter
    def priority(self, arg: int, /) -> None: ...

    @property
    def cpus(self) -> list[int]: ...

    @cpus.setter
    def cpus(self, arg: Sequence[int], /) -> None: ...

    @property
    def children(self) -> Children: ...

    @children.setter
    def children(self, arg: Children, /) -> None: ...

    def validate(self) -> None: ...

class Policy(enum.Enum):
    Inherit = 0

    Other = 1

    Fifo = 2

    RoundRobin = 3

class Report:
    @property
    def findings(self) -> list[Finding]: ...

    def ok(self) -> bool: ...

    def find(self, id: str) -> Finding: ...

    def to_string(self) -> str: ...

class Spec:
    @staticmethod
    def load(path: str | None = None) -> Spec: ...

    def empty(self) -> bool: ...

    def stages(self) -> list[Stage]: ...

    def stage(self, target: str, group: str = '') -> Stage: ...

    def find(self, target: str, group: str = '') -> Stage: ...

    def external(self) -> list[Stage]: ...

class Stage:
    @property
    def node(self) -> str: ...

    @property
    def group(self) -> str: ...

    @property
    def name(self) -> str: ...

    @property
    def kind(self) -> Kind: ...

    @property
    def external(self) -> bool: ...

    @property
    def expect_priority(self) -> int: ...

    @property
    def match(self) -> Match: ...

    @property
    def children_cpus(self) -> list[int]: ...

    @property
    def opts(self) -> Options: ...

    @property
    def strict(self) -> Strictness: ...

    @property
    def chain(self) -> str: ...

class Strictness(enum.Enum):
    Soft = 0

    Hard = 1

class ThreadState:
    @property
    def policy(self) -> int:
        """The raw SCHED_* number; compare os.SCHED_FIFO."""

    @property
    def priority(self) -> int: ...

    @property
    def cpus(self) -> list[int]: ...

    @property
    def reset_on_fork(self) -> bool: ...

    def __repr__(self) -> str: ...

class Verdict(enum.Enum):
    Ok = 0

    Warn = 1

    Fail = 2

    Unknown = 3

@overload
def apply_checked(opts: Options, strict: Strictness = Strictness.Soft, control_priority: int = 0) -> Report: ...

@overload
def apply_checked(stage: Stage) -> Report: ...

def bind(target: str, group: str = '') -> Report: ...

def bindings() -> list[Binding]: ...

def current() -> ThreadState: ...

def observe(tid: int) -> ThreadState: ...

@overload
def preflight(opts: Options, control_priority: int = 0) -> Report: ...

@overload
def preflight(spec: Spec) -> Report: ...

def this_tid() -> int:
    """The kernel tid; threading.get_ident() is not this number."""

def verify(stage: Stage, tid: int, window: datetime.timedelta = ...) -> Report: ...

@overload
def verify_process(spec: Spec) -> Report: ...

@overload
def verify_process(spec: Spec, pid: int) -> Report: ...
