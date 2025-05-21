extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <string>
#include <string_view>
#include <expected>
#include <chrono>
#include <fstream> 

#include <fmt/base.h>
#include <fmt/format.h>

using defReturn = std::expected<void, std::string>;

template <> class fmt::formatter<AVMediaType> {
public:
	constexpr auto parse(format_parse_context& ctx) { return ctx.begin(); }
	template <typename Context>
	constexpr auto format(AVMediaType const& foo, Context& ctx) const {
		return format_to(ctx.out(), "{}", static_cast<int>(foo));
	}
};

constexpr const std::string_view g_outFileName{ "screen_capture.mp4" };
constexpr const int32_t g_captureFrameRate{ 30 };
constexpr const char* g_captureResolution{ "1920x1080" };
constexpr const double g_recordDuration{ 10.0 };

constexpr const std::string_view g_deviceName{ "desktop" };
constexpr const std::string_view g_inputFormatName{ "gdigrab" };

static std::string ffmpeg_error_str(int32_t errnum) {
	char errbuf[AV_ERROR_MAX_STRING_SIZE];
	av_strerror(errnum, errbuf, sizeof(errbuf));
	return std::string(errbuf);
}

struct InputContext {
	AVFormatContext* fmt_ctx{ nullptr };
	int32_t video_stream_idx{ -1 };
	AVCodecParameters* video_codecpar{ nullptr };
};

struct OutputContext {
	AVFormatContext* fmt_ctx{ nullptr };
	AVStream* video_stream{ nullptr };
};

struct EncoderContext {
	AVCodecContext* codec_ctx{ nullptr };
	const AVCodec* codec{ nullptr };
	int64_t next_pts{ 0 };
};

struct ScalerContext {
	SwsContext* sws_ctx{ nullptr };
	AVFrame* scaled_frame{ nullptr };

};

inline void cleanup_input(InputContext& in_ctx) noexcept {
	if (in_ctx.fmt_ctx) {
		avformat_close_input(&in_ctx.fmt_ctx);
	}
}

inline void cleanup_output(OutputContext& out_ctx) noexcept {
	if (out_ctx.fmt_ctx) {
		if (!(out_ctx.fmt_ctx->oformat->flags & AVFMT_NOFILE) && out_ctx.fmt_ctx->pb != nullptr) {
			avio_closep(&out_ctx.fmt_ctx->pb);
		}
		avformat_free_context(out_ctx.fmt_ctx);
		out_ctx.fmt_ctx = nullptr;
	}
}

inline void cleanup_encoder(EncoderContext& enc_ctx) noexcept {
	if (enc_ctx.codec_ctx) {
		avcodec_free_context(&enc_ctx.codec_ctx);
	}
}

inline void cleanup_scaler(ScalerContext& scaler_ctx) noexcept {
	if (scaler_ctx.sws_ctx) {
		sws_freeContext(scaler_ctx.sws_ctx);
		scaler_ctx.sws_ctx = nullptr;
	}
	if (scaler_ctx.scaled_frame) {
		av_frame_free(&scaler_ctx.scaled_frame);
	}
}

inline void init_ffmpeg_libraries() noexcept
{
	avdevice_register_all();

}

inline defReturn init_device(InputContext& in_ctx)
{
	int32_t res{};
	AVDictionary* options{ nullptr };

	auto in_fmt = av_find_input_format(g_inputFormatName.data());
	if (!in_fmt) {
		return std::unexpected{
			fmt::format("Failed to find input format: {}", g_inputFormatName.data())
		};
	}

	av_dict_set(&options, "framerate", std::to_string(g_captureFrameRate).c_str(), 0);
	av_dict_set(&options, "video_size", g_captureResolution, 0);
	av_dict_set(&options, "draw_mouse", "1", 0);

	av_dict_set(&options, "probesize", "10M", 0);
	av_dict_set(&options, "analyzeduration", "10M", 0);

	in_ctx.fmt_ctx = avformat_alloc_context();
	if (!in_ctx.fmt_ctx) {
		av_dict_free(&options);
		return std::unexpected{
			fmt::format("Failed to allocate input format context: {}", ffmpeg_error_str(AVERROR(ENOMEM)))
		};
	}

	fmt::println("Opening input device '{}' with format '{}' and options...", g_deviceName.data(), g_inputFormatName.data());
	AVDictionaryEntry* t = nullptr;
	while ((t = av_dict_get(options, "", t, AV_DICT_IGNORE_SUFFIX))) {
		fmt::println("  Option: {} = {}", t->key, t->value);
	}

	if ((res = avformat_open_input(&in_ctx.fmt_ctx, g_deviceName.data(), in_fmt, &options)) < 0) {
		cleanup_input(in_ctx);
		av_dict_free(&options);
		return std::unexpected{
			fmt::format("Failed to open input device '{}': {}", g_deviceName.data(), ffmpeg_error_str(res))
		};
	}

	int ret = 0;
	if ((t = av_dict_get(options, "", nullptr, AV_DICT_IGNORE_SUFFIX))) {
		fmt::println(stderr, "Warning: Not all options were consumed by avformat_open_input:");
		while ((t = av_dict_get(options, "", t, AV_DICT_IGNORE_SUFFIX))) {
			fmt::println(stderr, "  Unused Option: {} = {}", t->key, t->value);
		}
	}
	av_dict_free(&options);

	if ((res = avformat_find_stream_info(in_ctx.fmt_ctx, nullptr)) < 0) {
		cleanup_input(in_ctx);
		return std::unexpected{
			fmt::format("Failed to find stream info: {}", ffmpeg_error_str(res))
		};
	}

	for (uint32_t i = 0; i < in_ctx.fmt_ctx->nb_streams; i++) {
		if (in_ctx.fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
			in_ctx.video_stream_idx = i;
			in_ctx.video_codecpar = in_ctx.fmt_ctx->streams[i]->codecpar;

			break;
		}
	}

	if (in_ctx.video_stream_idx == -1) {
		cleanup_input(in_ctx);
		return std::unexpected{ fmt::format("Failed to find video stream in input device.") };
	}

	fmt::println(
		"Input: {} ({})\n  Format: {}, Size: {}x{}, Input Stream Framerate (reported by device): {:.2f} fps, Input Stream Timebase: {}/{}",
		g_deviceName,
		g_inputFormatName,
		av_get_pix_fmt_name(static_cast<AVPixelFormat>(in_ctx.video_codecpar->format)),
		in_ctx.video_codecpar->width,
		in_ctx.video_codecpar->height,
		av_q2d(in_ctx.fmt_ctx->streams[in_ctx.video_stream_idx]->r_frame_rate),
		in_ctx.fmt_ctx->streams[in_ctx.video_stream_idx]->time_base.num,
		in_ctx.fmt_ctx->streams[in_ctx.video_stream_idx]->time_base.den
	);

	return {};
}

inline defReturn initialize_output(OutputContext& out_ctx, const InputContext& in_ctx)
{
	int32_t res{};
	if ((res = avformat_alloc_output_context2(&out_ctx.fmt_ctx, nullptr, nullptr, g_outFileName.data())) < 0) {
		return std::unexpected{
			fmt::format("Failed to allocate output format context for '{}': {}", g_outFileName.data(), ffmpeg_error_str(res))
		};
	}

	out_ctx.video_stream = avformat_new_stream(out_ctx.fmt_ctx, nullptr);
	if (!out_ctx.video_stream) {
		cleanup_output(out_ctx);
		return std::unexpected{ fmt::format("Failed to create new stream: {}", ffmpeg_error_str(AVERROR(ENOMEM))) };
	}

	out_ctx.video_stream->time_base = av_make_q(1, g_captureFrameRate);

	if (!(out_ctx.fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
		if ((res = avio_open(&out_ctx.fmt_ctx->pb, g_outFileName.data(), AVIO_FLAG_WRITE)) < 0) {
			cleanup_output(out_ctx);
			return std::unexpected{
				fmt::format("Failed to open output file '{}': {}", g_outFileName.data(), ffmpeg_error_str(res))
			};
		}
	}

	fmt::println("Output: {} ({})\n  Target Size: {}x{}, Target Framerate (set): {}",
		g_outFileName,
		out_ctx.fmt_ctx->oformat->long_name ? out_ctx.fmt_ctx->oformat->long_name : out_ctx.fmt_ctx->oformat->name,
		in_ctx.video_codecpar->width,
		in_ctx.video_codecpar->height,
		g_captureFrameRate
	);
	return {};
}

inline defReturn initialize_encoder(EncoderContext& enc_ctx, OutputContext& out_ctx, const InputContext& in_ctx)
{
	int32_t res{};
	const char* encoder_names[] = { "h264_nvenc", "libx264", nullptr };

	for (const char** name = encoder_names; *name != nullptr; ++name) {
		enc_ctx.codec = avcodec_find_encoder_by_name(*name);
		if (enc_ctx.codec) {
			fmt::println("Found encoder: {}", *name);
			break;
		}
	}
	if (!enc_ctx.codec) {
		return std::unexpected{ fmt::format("Failed to find a suitable H.264 encoder.") };
	}

	enc_ctx.codec_ctx = avcodec_alloc_context3(enc_ctx.codec);
	if (!enc_ctx.codec_ctx) {
		return std::unexpected{ fmt::format("Failed to allocate encoder context: {}", ffmpeg_error_str(AVERROR(ENOMEM))) };
	}

	enc_ctx.codec_ctx->height = in_ctx.video_codecpar->height;
	enc_ctx.codec_ctx->width = in_ctx.video_codecpar->width;
	enc_ctx.codec_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
	enc_ctx.codec_ctx->time_base = out_ctx.video_stream->time_base;
	enc_ctx.codec_ctx->framerate = av_make_q(g_captureFrameRate, 1);
	enc_ctx.codec_ctx->bit_rate = 9000000;
	enc_ctx.codec_ctx->gop_size = g_captureFrameRate;

	if (std::string_view(enc_ctx.codec->name) == "h264_nvenc") {
		fmt::println("Configuring h264_nvenc options...");
		av_opt_set(enc_ctx.codec_ctx->priv_data, "preset", "p1", 0);
		av_opt_set(enc_ctx.codec_ctx->priv_data, "tune", "ll", 0);

	}
	else if (std::string_view(enc_ctx.codec->name) == "libx264") {
		fmt::println("Configuring libx264 options...");
		av_opt_set(enc_ctx.codec_ctx->priv_data, "preset", "ultrafast", 0);
		av_opt_set(enc_ctx.codec_ctx->priv_data, "tune", "zerolatency", 0);
	}

	if (out_ctx.fmt_ctx->oformat->flags & AVFMT_GLOBALHEADER) {
		enc_ctx.codec_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
	}

	if ((res = avcodec_open2(enc_ctx.codec_ctx, enc_ctx.codec, nullptr)) < 0) {
		cleanup_encoder(enc_ctx);
		return std::unexpected{ fmt::format("Failed to open encoder '{}': {}", enc_ctx.codec->name, ffmpeg_error_str(res)) };
	}

	if ((res = avcodec_parameters_from_context(out_ctx.video_stream->codecpar, enc_ctx.codec_ctx)) < 0) {
		cleanup_encoder(enc_ctx);
		return std::unexpected{ fmt::format("Failed to copy codec parameters to output stream: {}", ffmpeg_error_str(res)) };
	}

	fmt::println(
		"Encoder: {}\n  Output Format: {}, Size: {}x{}, Target Pixel Format: {}, Framerate (set): {}",
		enc_ctx.codec->long_name ? enc_ctx.codec->long_name : enc_ctx.codec->name,
		out_ctx.fmt_ctx->oformat->name,
		enc_ctx.codec_ctx->width, enc_ctx.codec_ctx->height,
		av_get_pix_fmt_name(enc_ctx.codec_ctx->pix_fmt), g_captureFrameRate
	);
	return {};
}

inline defReturn initialize_scaler(ScalerContext& scaler_ctx, const InputContext& in_ctx, const EncoderContext& enc_ctx)
{
	scaler_ctx.sws_ctx = sws_getContext(
		in_ctx.video_codecpar->width, in_ctx.video_codecpar->height, (AVPixelFormat)in_ctx.video_codecpar->format,
		enc_ctx.codec_ctx->width, enc_ctx.codec_ctx->height, enc_ctx.codec_ctx->pix_fmt,
		SWS_BILINEAR, nullptr, nullptr, nullptr
	);
	if (!scaler_ctx.sws_ctx) {
		return std::unexpected{ fmt::format("Failed to create scaler context ({} {}x{} -> {} {}x{})",
			av_get_pix_fmt_name(static_cast<AVPixelFormat>(in_ctx.video_codecpar->format)),
			in_ctx.video_codecpar->width, in_ctx.video_codecpar->height,
			av_get_pix_fmt_name(enc_ctx.codec_ctx->pix_fmt),
			enc_ctx.codec_ctx->width, enc_ctx.codec_ctx->height) };
	}

	scaler_ctx.scaled_frame = av_frame_alloc();
	if (!scaler_ctx.scaled_frame) {
		cleanup_scaler(scaler_ctx);
		return std::unexpected{ fmt::format("Failed to allocate scaled frame: {}", ffmpeg_error_str(AVERROR(ENOMEM))) };
	}

	scaler_ctx.scaled_frame->format = enc_ctx.codec_ctx->pix_fmt;
	scaler_ctx.scaled_frame->width = enc_ctx.codec_ctx->width;
	scaler_ctx.scaled_frame->height = enc_ctx.codec_ctx->height;

	int32_t res = av_image_alloc(scaler_ctx.scaled_frame->data, scaler_ctx.scaled_frame->linesize,
		enc_ctx.codec_ctx->width, enc_ctx.codec_ctx->height, enc_ctx.codec_ctx->pix_fmt, 32);
	if (res < 0) {
		cleanup_scaler(scaler_ctx);
		return std::unexpected{ fmt::format("Failed to allocate scaled frame buffer: {}", ffmpeg_error_str(res)) };
	}

	fmt::println(
		"Scaler: Initialized\n  From: {} {}x{}\n  To: {} {}x{}",
		av_get_pix_fmt_name(static_cast<AVPixelFormat>(in_ctx.video_codecpar->format)),
		in_ctx.video_codecpar->width, in_ctx.video_codecpar->height,
		av_get_pix_fmt_name(static_cast<AVPixelFormat>(scaler_ctx.scaled_frame->format)),
		scaler_ctx.scaled_frame->width, scaler_ctx.scaled_frame->height
	);
	return {};
}

inline defReturn encode_and_write_frame(AVFrame* frame_to_encode, EncoderContext& enc_ctx, OutputContext& out_ctx)
{
	int32_t ret{};
	AVPacket* pkt = av_packet_alloc();
	if (!pkt) {
		return std::unexpected{ fmt::format("Failed to allocate packet: {}", ffmpeg_error_str(AVERROR(ENOMEM))) };
	}

	if (frame_to_encode) {
		frame_to_encode->pts = enc_ctx.next_pts++;
	}

	ret = avcodec_send_frame(enc_ctx.codec_ctx, frame_to_encode);
	if (ret < 0) {
		av_packet_free(&pkt);
		return std::unexpected{ fmt::format("Failed to send frame to encoder: {}", ffmpeg_error_str(ret)) };
	}

	while (true) {
		ret = avcodec_receive_packet(enc_ctx.codec_ctx, pkt);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
			break;
		}
		else if (ret < 0) {
			av_packet_free(&pkt);
			return std::unexpected{ fmt::format("Error receiving packet from encoder: {}", ffmpeg_error_str(ret)) };
		}

		av_packet_rescale_ts(pkt, enc_ctx.codec_ctx->time_base, out_ctx.video_stream->time_base);
		pkt->stream_index = out_ctx.video_stream->index;

		int32_t write_ret = av_interleaved_write_frame(out_ctx.fmt_ctx, pkt);

		if (write_ret < 0) {
			av_packet_unref(pkt);
			av_packet_free(&pkt);
			return std::unexpected{ fmt::format("Failed to write frame to output: {}", ffmpeg_error_str(write_ret)) };
		}
		av_packet_unref(pkt);
	}
	av_packet_free(&pkt);
	return {};
}

int main() {
	InputContext in_ctx{};
	OutputContext out_ctx{};
	EncoderContext enc_ctx{};
	ScalerContext scaler_ctx{};
	int32_t res{};

	init_ffmpeg_libraries();

	auto device_result = init_device(in_ctx);
	if (!device_result) {
		fmt::println(stderr, "Error initializing device: {}", device_result.error());
		return 1;
	}

	auto output_result = initialize_output(out_ctx, in_ctx);
	if (!output_result) {
		fmt::println(stderr, "Error initializing output: {}", output_result.error());
		cleanup_input(in_ctx);
		return 1;
	}

	auto encoder_result = initialize_encoder(enc_ctx, out_ctx, in_ctx);
	if (!encoder_result) {
		fmt::println(stderr, "Error initializing encoder: {}", encoder_result.error());
		cleanup_output(out_ctx);
		cleanup_input(in_ctx);
		return 1;
	}

	bool scaler_initialized = false;
	if (static_cast<AVPixelFormat>(in_ctx.video_codecpar->format) != enc_ctx.codec_ctx->pix_fmt ||
		in_ctx.video_codecpar->width != enc_ctx.codec_ctx->width ||
		in_ctx.video_codecpar->height != enc_ctx.codec_ctx->height) {
		auto scaler_result = initialize_scaler(scaler_ctx, in_ctx, enc_ctx);
		if (!scaler_result) {
			fmt::println(stderr, "Error initializing scaler: {}", scaler_result.error());
			cleanup_encoder(enc_ctx); cleanup_output(out_ctx); cleanup_input(in_ctx);
			return 1;
		}
		scaler_initialized = true;
	}
	else {
		fmt::println("Input and output formats/dimensions match, no scaling required.");
	}

	if ((res = avformat_write_header(out_ctx.fmt_ctx, nullptr)) < 0) {
		fmt::println(stderr, "Error writing output header: {}", ffmpeg_error_str(res));
		if (scaler_initialized) cleanup_scaler(scaler_ctx);
		cleanup_encoder(enc_ctx); cleanup_output(out_ctx); cleanup_input(in_ctx);
		return 1;
	}

	AVPacket* in_pkt = av_packet_alloc();
	AVFrame* raw_frame = av_frame_alloc();
	if (!in_pkt || !raw_frame) {
		fmt::println(stderr, "Error allocating packet/frame for input: {}", ffmpeg_error_str(AVERROR(ENOMEM)));
		if (scaler_initialized) cleanup_scaler(scaler_ctx);
		cleanup_encoder(enc_ctx); cleanup_output(out_ctx); cleanup_input(in_ctx);
		if (out_ctx.fmt_ctx && !(out_ctx.fmt_ctx->oformat->flags & AVFMT_NOFILE) && out_ctx.fmt_ctx->pb) {
			av_write_trailer(out_ctx.fmt_ctx);
		}
		return 1;
	}

	fmt::println("Recording started for {} seconds (0 for indefinite)...", g_recordDuration);
	auto start_time = std::chrono::steady_clock::now();
	int32_t frames_written{};
	int32_t packets_read_count{};
	bool first_packet_dumped = false;

	const int32_t GDI_HEADER_SIZE = 54;

	while (true) {
		auto current_time = std::chrono::steady_clock::now();
		std::chrono::duration<double> elapsed_seconds = current_time - start_time;
		if (g_recordDuration > 0.0 && elapsed_seconds.count() >= g_recordDuration) {
			fmt::println("Recording duration ({:.1f}s) reached.", g_recordDuration);
			break;
		}

		res = av_read_frame(in_ctx.fmt_ctx, in_pkt);
		if (res < 0) {
			if (res == AVERROR_EOF) {
				fmt::println("End of input stream reached.");
			}
			else {
				fmt::println(stderr, "Error reading frame from input: {}", ffmpeg_error_str(res));
			}
			break;
		}
		packets_read_count++;

		if (in_pkt->stream_index == in_ctx.video_stream_idx) {

			const int32_t expected_frame_size = in_ctx.video_codecpar->width * in_ctx.video_codecpar->height * 4;

			if (in_pkt->size < (expected_frame_size + GDI_HEADER_SIZE)) {
				fmt::println(
					stderr,
					"Warning: Video packet #{} size {} is too small for header ({}) + expected frame ({}). Total expected: {}. Skipping.",
					packets_read_count,
					in_pkt->size,
					GDI_HEADER_SIZE,
					expected_frame_size,
					expected_frame_size + GDI_HEADER_SIZE
				);
				av_packet_unref(in_pkt);
				continue;
			}

			if (in_pkt->size > (expected_frame_size + GDI_HEADER_SIZE)) {
				fmt::println(
					"Note: Video packet #{} size {} is slightly larger than header + expected frame ({}). Using it.",
					packets_read_count,
					in_pkt->size,
					expected_frame_size + GDI_HEADER_SIZE
				);
			}

			raw_frame->format = static_cast<AVPixelFormat>(in_ctx.video_codecpar->format);
			raw_frame->width = in_ctx.video_codecpar->width;
			raw_frame->height = in_ctx.video_codecpar->height;

			raw_frame->data[0] = in_pkt->data + GDI_HEADER_SIZE;

			raw_frame->linesize[0] = in_ctx.video_codecpar->width * 4;
			for (int i = 1; i < AV_NUM_DATA_POINTERS; ++i) {
				raw_frame->data[i] = nullptr;
				raw_frame->linesize[i] = 0;
			}

			AVFrame* frame_to_encode = raw_frame;
			if (scaler_initialized) {
				res = sws_scale(
					scaler_ctx.sws_ctx,
					(const uint8_t* const*)raw_frame->data,
					raw_frame->linesize,
					0,
					in_ctx.video_codecpar->height,
					scaler_ctx.scaled_frame->data,
					scaler_ctx.scaled_frame->linesize
				);
				if (res < 0) {
					fmt::println(stderr, "Error scaling frame: {}", ffmpeg_error_str(res));
					av_packet_unref(in_pkt);
					continue;
				}
				if (res != enc_ctx.codec_ctx->height) {
					fmt::println(
						stderr,
						"Warning: sws_scale output {} lines, expected {}",
						res,
						enc_ctx.codec_ctx->height
					);
				}
				frame_to_encode = scaler_ctx.scaled_frame;
			}

			auto encode_result = encode_and_write_frame(frame_to_encode, enc_ctx, out_ctx);
			if (!encode_result) {
				fmt::println(stderr, "Error encoding and writing frame: {}", encode_result.error());

			}
			else {
				frames_written++;
			}
		}
		av_packet_unref(in_pkt);
	}

	fmt::println("Flushing encoder...");
	auto flush_result = encode_and_write_frame(nullptr, enc_ctx, out_ctx);
	if (!flush_result) fmt::println(stderr, "Error flushing encoder: {}", flush_result.error());
	else fmt::println("Encoder flushed successfully.");

	if (out_ctx.fmt_ctx && !(out_ctx.fmt_ctx->oformat->flags & AVFMT_NOFILE) && out_ctx.fmt_ctx->pb) {
		if ((res = av_write_trailer(out_ctx.fmt_ctx)) < 0) {
			fmt::println(stderr, "Error writing output trailer: {}", ffmpeg_error_str(res));
		}
		else {
			fmt::println("Trailer written successfully.");
		}
	}

	fmt::println("Cleaning up resources...");
	av_packet_free(&in_pkt);
	av_frame_free(&raw_frame);

	if (scaler_initialized) cleanup_scaler(scaler_ctx);
	cleanup_encoder(enc_ctx);
	cleanup_output(out_ctx);
	cleanup_input(in_ctx);

	fmt::println("Screen recording finished. {} frames written to {}. Total packets read from input: {}",
		frames_written, g_outFileName, packets_read_count);
	return 0;
}