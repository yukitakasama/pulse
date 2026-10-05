#include "../preview_host/preview_integrity.h"
#include <cstdio>

int main() {
    using namespace pulse::preview;
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
        if (!ok) ++failures;
    };
    DecodeResult result;
    check(DescribeIntegrity(result, true).state == IntegrityState::Complete, "successful native scope is complete");
    result.decoder = "shell-thumbnail";
    check(DescribeIntegrity(result, true).state == IntegrityState::Unknown, "opaque provider is not declared complete");
    result = {};
    result.error = L"udf-unsupported-partition";
    check(DescribeIntegrity(result, false).state == IntegrityState::Unsupported, "unsupported UDF is distinct from read failure");
    result.error = L"udf-invalid";
    check(DescribeIntegrity(result, false).state == IntegrityState::Failed, "damaged UDF is a failure");
    result.text = L"PULSEARC\t1\tISO/UDF\t1000\t1\n0\t-\t14\t-\t-\treadme.txt\n";
    auto status = DescribeIntegrity(result, true);
    check(status.state == IntegrityState::Partial && status.loaded == 1 && status.total == 0,
        "compatibility directory keeps loaded count and unknown total");
    result = {};
    result.text = L"PULSETBL\t1\nW\t100\t1\t1\nS\txlsx\tOne\t0\t0\t0\tnot-loaded\t0\nS\txlsx\tTwo\t1\t1\t0\t\t0\n";
    status = DescribeIntegrity(result, true);
    check(status.reason == IntegrityReason::OnDemand && status.loaded == 1 && status.total == 100,
        "unrequested sheets are on-demand rather than errors");
    result.text = L"PULSETBL\t1\nW\t100\t1\t0\nS\txlsx\tOne\t1\t100\t1\t\t0\n";
    check(DescribeIntegrity(result, true).reason == IntegrityReason::Limit, "selected-sheet limit takes precedence over on-demand");
    result.text = L"PULSETBL\t1\nW\t100\t0\t0\nS\txlsx\tOne\t0\t0\t1\tread-failed\t0\n";
    check(DescribeIntegrity(result, true).reason == IntegrityReason::ReadFailure, "selected-sheet failure takes precedence over on-demand");
    result = {};
    result.error = L"svg-unsupported:text";
    check(DescribeIntegrity(result, true).reason == IntegrityReason::SourceFallback, "SVG source fallback stays partial");
    result = {};
    result.error = L"image-preview-incomplete:frame-limit";
    result.text = L"PULSEIMAGE\t1\nT\tanimation\nF\t4096\t4097\t0\n";
    status = DescribeIntegrity(result, true);
    check(status.loaded == 4096 && status.total == 4097 && status.reason == IntegrityReason::Limit,
        "APNG reports retained and declared frames");
    result = {};
    result.text = L"PULSEMD\t1\nV\tchapters\t4\t5\n";
    status = DescribeIntegrity(result, true);
    check(status.unit == IntegrityUnit::Chapters && status.loaded == 4 && status.total == 5 &&
        status.state == IntegrityState::Partial, "EPUB chapter omissions have structured counts");
    check(IntegrityNumber(L"184467440737095516160") == 0, "overflow cannot fabricate a count");
    result = {};
    result.kind = pulse::ipc::PreviewContentKind::Hex;
    check(DescribeIntegrity(result, true).reason == IntegrityReason::SourceFallback,
        "binary fallback never claims complete format rendering");
    return failures ? 1 : 0;
}
