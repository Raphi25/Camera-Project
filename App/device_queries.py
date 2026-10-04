"""Read-only device queries returning presentation-ready domain results."""

from dataclasses import dataclass
from collections.abc import Callable

from presentation_services import (
    DashboardViewModel,
    SummaryListViewModel,
    build_dashboard_view,
    build_summary_list,
    build_summary_text,
)


Request = Callable[[str, Callable[[str], bool], int, bool], list[str]]


@dataclass(frozen=True)
class DashboardQueryResult:
    view: DashboardViewModel | None = None
    cancelled: bool = False
    error: str | None = None


@dataclass(frozen=True)
class SummaryListQueryResult:
    view: SummaryListViewModel | None = None
    error: str | None = None


@dataclass(frozen=True)
class SummaryTextQueryResult:
    name: str
    text: str | None = None
    error: str | None = None


class DeviceQueryService:
    """Pair related firmware requests and preserve errors for the UI."""
    def __init__(
        self,
        request: Request,
        should_cancel: Callable[[], bool] = lambda: False,
    ) -> None:
        self._request = request
        self._should_cancel = should_cancel

    def dashboard(self, log_requests: bool = True) -> DashboardQueryResult:
        """Fetch STATUS and SD_STATUS as one cancellable dashboard snapshot."""
        try:
            status_lines = self._request(
                "STATUS",
                lambda line: (
                    line.startswith("STATUS ") or line.startswith("ERR STATUS")
                ),
                900,
                log_requests,
            )
            if self._should_cancel():
                return DashboardQueryResult(cancelled=True)
            sd_lines = self._request(
                "SD_STATUS",
                lambda line: (
                    line.startswith("SD_STATUS ")
                    or line.startswith("ERR SD_STATUS")
                ),
                900,
                log_requests,
            )
            if self._should_cancel():
                return DashboardQueryResult(cancelled=True)
            return DashboardQueryResult(build_dashboard_view(status_lines, sd_lines))
        except Exception as exc:
            return DashboardQueryResult(error=str(exc))

    def summary_list(self) -> SummaryListQueryResult:
        try:
            lines = self._request(
                "SUMMARY_LIST",
                lambda line: (
                    line.startswith("OK SUMMARY_LIST")
                    or line.startswith("ERR SUMMARY_LIST")
                ),
                4000,
                True,
            )
            return SummaryListQueryResult(build_summary_list(lines))
        except Exception as exc:
            return SummaryListQueryResult(error=str(exc))

    def summary_text(self, name: str) -> SummaryTextQueryResult:
        try:
            lines = self._request(
                f"SUMMARY_GET {name}",
                lambda line: (
                    line.startswith("END_SUMMARY ")
                    or line.startswith("ERR SUMMARY_GET")
                ),
                5000,
                True,
            )
            return SummaryTextQueryResult(name, build_summary_text(name, lines))
        except Exception as exc:
            return SummaryTextQueryResult(name, error=str(exc))
