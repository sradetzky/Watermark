#include "app/jobs.h"
#include "app/files.h"
#include "core/internal.h"

#include <algorithm>
#include <iomanip>
#include <sstream>

namespace watermark::app {
namespace fs = std::filesystem;
namespace {
void protect(const Job& job, const std::vector<fs::path>& files, const fs::path& destination) {
    for (const auto& path : {job.source, job.mark, job.key_file, job.visible_image}) {
        if (!path.empty() && same_path(path, destination)) {
            throw std::invalid_argument("Output cannot overwrite the source, visible image, manifest, or key file.");
        }
    }
    for (const auto& path : files) {
        if (same_path(path, destination)) { throw std::invalid_argument("Output cannot overwrite an input image."); }
    }
}
std::string detection_row(const fs::path& file, const DetectionResult& result) {
    std::ostringstream row;
    row << std::fixed << std::setprecision(6) << csv(display(file)) << ',' << verdict_name(result.verdict) << ','
        << result.bit_accuracy.value_or(0) << ',' << result.confidence << ',';
    if (result.recovered_hash) { row << hash_text(*result.recovered_hash); }
    row << ',';
    if (result.hash_matches) { row << (*result.hash_matches ? "true" : "false"); }
    row << ',' << result.scale << ",," << result.offset_x << ',' << result.offset_y << '\n';
    return row.str();
}
}
void export_report(const Job& job, const fs::path& destination, const std::string& text) {
    const auto files = input_files(fs::absolute(job.input), job.recursive, {}, {});
    protect(job, files, destination);
    write_text(destination, text, true);
}
Summary run_job(const Job& job, const Progress& progress) {
    if (job.input.empty() || (job.embedding && job.output.empty()) ||
        (job.embedding ? job.source.empty() : job.source.empty() == job.mark.empty())) {
        throw std::invalid_argument("Choose an input image/folder and watermark image or manifest; embedding also needs an output folder.");
    }
    if (job.jpeg_quality < 1 || job.jpeg_quality > 100) { throw std::invalid_argument("JPEG quality must be 1..100."); }
    auto params = job.parameters;
    if (!job.key_file.empty() && params.passphrase.empty()) { params.passphrase = read_key_file(job.key_file); }
    if (job.key_from_source && (!job.key_file.empty() || !params.passphrase.empty())) {
        throw std::invalid_argument("Watermark identity mode cannot also use a passphrase or key file.");
    }
    Summary summary;
    if (params.cancelled && params.cancelled()) { summary.cancelled = true; return summary; }
    const auto input = fs::absolute(job.input);
    const auto output = job.embedding ? fs::absolute(job.output) : fs::path{};
    if (job.embedding && fs::is_directory(input) && same_path(input, output)) {
        throw std::invalid_argument("Output folder must differ from the input folder.");
    }
    const auto files = input_files(input, job.recursive, output, progress, params.cancelled);
    summary.total = files.size();
    bool source_key = job.key_from_source;
    Payload payload;
    if (job.source.empty()) {
        const auto mark = read_mark(job.mark);
        payload = mark.payload;
        if (mark.key_from_source && (!job.key_file.empty() || !params.passphrase.empty())) {
            throw std::invalid_argument("This manifest uses watermark identity; omit the passphrase/key file.");
        }
        if (!mark.key_from_source && source_key) {
            throw std::invalid_argument("This legacy manifest requires its original passphrase or key file.");
        }
        source_key = mark.key_from_source;
    } else { payload = payload_from_source(load_image(job.source), source_key); }
    if (source_key) { params.passphrase = "Watermark/source-key/v1/" + hash_text(payload.source_hash); }
    detail::validate_parameters(params);
    std::optional<Image> silhouette;
    if (!job.visible_image.empty()) {
        if (!job.embedding) { throw std::invalid_argument("Visible watermark options apply only to embedding."); }
        silhouette = load_image(job.visible_image);
        // Validate the cutout and settings before creating a manifest or processing files.
        apply_visible_watermark(Image(128, 128), *silhouette, job.visible, params.cancelled);
    }
    const auto manifest = output / "mark.json";
    fs::path report = job.report;
    if (!job.embedding && job.automatic_report && report.empty() && fs::is_directory(input)) { report = "report.csv"; }
    if (job.embedding) {
        protect(job, files, manifest);
        fs::create_directories(output);
        if (fs::exists(manifest) && !job.force) {
            const auto existing = read_mark(manifest);
            if (existing.payload.source_hash != payload.source_hash || existing.key_from_source != source_key) {
                throw std::invalid_argument("Output mark.json belongs to another source or key mode; choose another folder or force a rewrite.");
            }
        }
        detail::check_cancelled(params);
        write_text(manifest, mark_json(payload, params.strength, source_key), true);
    } else if (!report.empty()) { protect(job, files, report); }
    summary.csv = "file,verdict,bit_accuracy,confidence,recovered_hash,hash_matches,scale,error,offset_x,offset_y\n";
    for (const auto& file : files) {
        if (params.cancelled && params.cancelled()) { summary.cancelled = true; break; }
        if (progress) { progress({file, "Processing " + display(file), {}, false, summary.completed, summary.total}); }
        Event event{file, {}, {}, false, summary.completed, summary.total};
        try {
            const auto image = load_image(file);
            if (job.embedding) {
                const auto relative = fs::is_directory(input) ? fs::relative(file, input) : file.filename();
                auto destination = output / relative;
                destination += job.format == ImageFormat::png ? L".png" : L".jpg";
                protect(job, files, destination);
                if (!job.force) {
                    const auto existing = detect(image, payload, params);
                    if (existing.verdict != Verdict::absent && existing.hash_matches == true) {
                        ++summary.skipped;
                        event.message = "SKIP already marked " + display(file);
                    } else if (fs::exists(destination)) {
                        throw std::runtime_error("Output exists; force a rewrite to replace it.");
                    }
                }
                if (event.message.empty()) {
                    const auto prepared = silhouette ? apply_visible_watermark(image, *silhouette, job.visible, params.cancelled) : image;
                    const auto marked = embed(prepared, payload, params);
                    detail::check_cancelled(params);
                    fs::create_directories(destination.parent_path());
                    write_image(marked, destination, job.format, job.jpeg_quality, job.force);
                    ++summary.embedded;
                    std::ostringstream text;
                    text << "EMBED " << display(destination) << (silhouette ? " visible + keyed PSNR=" : " PSNR=")
                         << psnr(prepared, marked) << " dB";
                    event.message = text.str();
                }
            } else {
                event.detection = detect(image, payload, params);
                summary.csv += detection_row(file, *event.detection);
                const auto& result = *event.detection;
                std::ostringstream text;
                text << verdict_name(result.verdict) << ' ' << display(file) << " bits=" << result.bit_accuracy.value_or(0)
                     << " confidence=" << result.confidence;
                if (result.hash_matches) { text << " match=" << (*result.hash_matches ? "true" : "false"); }
                event.message = text.str();
            }
        } catch (const OperationCancelled&) {
            summary.cancelled = true; break;
        } catch (const std::exception& error) {
            ++summary.errors;
            event.error = true;
            event.message = "ERROR " + display(file) + ": " + error.what();
            if (!job.embedding) { summary.csv += csv(display(file)) + ",error,,,,,," + csv(error.what()) + ",,\n"; }
        }
        event.completed = ++summary.completed;
        if (progress) { progress(event); }
    }
    if (!job.embedding && !report.empty()) {
        write_text(report, summary.csv, true);
        if (progress) { progress({report, "Report " + display(report), {}, false, summary.completed, summary.total}); }
    }
    return summary;
}
} // namespace watermark::app
