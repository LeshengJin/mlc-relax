/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */
/*!
 * \file src/runtime/relax_vm/audio_support.cc
 * \brief Runtime to support ASR/TTS models
 */
#include <tvm/runtime/ndarray.h>
#include <tvm/runtime/relax_vm/vm.h>

namespace tvm {
namespace runtime {
namespace relax_vm {

void hanning_window(std::vector<double>& window, int M) {
  for (size_t i = 0; i < window.size(); i++) {
    window[i] = 0.5 - 0.5 * std::cos(2 * M_PI * i / (M - 1));
  }
}

void dft(const std::vector<double>& in, std::vector<double>& out) {
  int N = in.size();

  out.resize(N * 2);

  for (int k = 0; k < N; k++) {
    float re = 0;
    float im = 0;

    for (int n = 0; n < N; n++) {
      float angle = 2 * M_PI * k * n / N;
      re += in[n] * cos(angle);
      im -= in[n] * sin(angle);
    }

    out[k * 2 + 0] = re;
    out[k * 2 + 1] = im;
  }
}

void fft(const std::vector<double>& in, std::vector<double>& out) {
  out.resize(in.size() * 2);

  int N = in.size();

  if (N == 1) {
    out[0] = in[0];
    out[1] = 0;
    return;
  }

  if (N % 2 == 1) {
    dft(in, out);
    return;
  }

  std::vector<double> even;
  std::vector<double> odd;

  even.reserve(N / 2);
  odd.reserve(N / 2);

  for (int i = 0; i < N; i++) {
    if (i % 2 == 0) {
      even.push_back(in[i]);
    } else {
      odd.push_back(in[i]);
    }
  }

  std::vector<double> even_fft;
  std::vector<double> odd_fft;

  fft(even, even_fft);
  fft(odd, odd_fft);

  for (int k = 0; k < N / 2; k++) {
    float theta = 2 * M_PI * k / N;

    float re = cos(theta);
    float im = -sin(theta);

    float re_odd = odd_fft[2 * k + 0];
    float im_odd = odd_fft[2 * k + 1];

    out[2 * k + 0] = even_fft[2 * k + 0] + re * re_odd - im * im_odd;
    out[2 * k + 1] = even_fft[2 * k + 1] + re * im_odd + im * re_odd;

    out[2 * (k + N / 2) + 0] = even_fft[2 * k + 0] - re * re_odd + im * im_odd;
    out[2 * (k + N / 2) + 1] = even_fft[2 * k + 1] - re * im_odd - im * re_odd;
  }
}

void get_mel_filters(int sr, int n_fft, int n_mels, std::vector<std::vector<double>>& filters) {
  std::vector<double> fftfreqs;
  double val = 1.0 / (n_fft * 1.0 / sr);
  int N = n_fft / 2 + 1;
  fftfreqs.resize(N);
  for (size_t i = 0; i < N; ++i) {
    fftfreqs[i] = i * val;
  }

  double min_mel = 0.0;
  double max_mel = 45.245640471924965;
  double melstep = (max_mel - min_mel) / double(n_mels + 1);

  std::vector<double> mels;
  mels.resize(n_mels + 2);
  for (size_t i = 0; i < n_mels + 2; ++i) {
    mels[i] = min_mel + i * melstep;
  }

  double f_min = 0.0;
  double f_sp = 200.0 / 3;
  std::vector<double> freqs;
  freqs.resize(n_mels + 2);
  for (size_t i = 0; i < n_mels + 2; ++i) {
    freqs[i] = f_min + f_sp * mels[i];
  }

  double min_log_hz = 1000.0;
  double min_log_mel = (min_log_hz - f_min) / f_sp;
  double logstep = std::log(6.4) / 27.0;

  for (size_t i = 0; i < n_mels + 2; ++i) {
    if (mels[i] >= min_log_mel) {
      freqs[i] = min_log_hz * std::exp(logstep * (mels[i] - min_log_mel));
    }
  }

  std::vector<double> fdiff;
  fdiff.resize(n_mels + 1);
  for (size_t i = 0; i < n_mels + 1; ++i) {
    fdiff[i] = freqs[i + 1] - freqs[i];
  }

  std::vector<std::vector<double>> ramps;
  ramps.resize(freqs.size());

  for (size_t i = 0; i < freqs.size(); ++i) {
    ramps[i].resize(fftfreqs.size());
    for (size_t j = 0; j < fftfreqs.size(); ++j) {
      ramps[i][j] = freqs[i] - fftfreqs[j];
    }
  }

  filters.resize(ramps.size() - 2);
  for (size_t i = 0; i < ramps.size() - 2; i++) {
    filters[i].resize(ramps[0].size());
    double enorm = 2.0 / (freqs[i + 2] - freqs[i]);
    for (size_t j = 0; j < ramps[0].size(); ++j) {
      double lower = -ramps[i][j] / fdiff[i];
      double upper = ramps[i + 2][j] / fdiff[i + 1];
      filters[i][j] = enorm * std::max(0.0, std::min(lower, upper));
    }
  }
}

void log_mel_spec(std::vector<double>& sample_data, int num_id, std::vector<double>& window,
                  int n_fft, int hop_length, std::vector<std::vector<double>> mel_filters,
                  std::vector<std::vector<double>>& log_mel_spec) {
  std::vector<double> frame;

  frame.resize(n_fft);
  for (size_t i = 0; i < n_fft; i++) {
    frame[i] = sample_data[num_id * hop_length + i] * window[i];
  }

  std::vector<double> fft_out;
  fft(frame, fft_out);

  for (size_t i = 0; i < n_fft; i++) {
    fft_out[i] = fft_out[2 * i] * fft_out[2 * i] + fft_out[2 * i + 1] * fft_out[2 * i + 1];
  }

  for (size_t i = 0; i < n_fft / 2 + 1; i++) {
    fft_out[i] = 0.5 * (fft_out[i] + fft_out[n_fft - i]);
  }

  for (size_t i = 0; i < mel_filters.size(); i++) {
    double matmul_result = 0.0;
    for (size_t k = 0; k < n_fft / 2 + 1; k++) {
      matmul_result += fft_out[k] * mel_filters[i][k];
    }
    matmul_result = std::max(matmul_result, 1e-10);
    matmul_result = std::log10(matmul_result);
    log_mel_spec[num_id][i] = matmul_result;
  }
}

NDArray WhisperProcessAudio(NDArray raw_speech) {
  ICHECK(raw_speech.IsContiguous());
  ICHECK(raw_speech.DataType() == DataType::Float(32)) << "raw speech data type is not float32!";
  ICHECK(raw_speech->device.device_type == kDLCPU) << "raw speech device must be CPU!";
  ICHECK_EQ(raw_speech->ndim, 1);

  const float* p_data = static_cast<float*>(raw_speech->data);

  int sampling_rate = 16000;
  int n_fft = 400;
  int n_mels = 80;
  int max_length = 480000;
  int hop_length = 160;

  std::vector<std::vector<double>> mel_filters;
  get_mel_filters(sampling_rate, n_fft, n_mels, mel_filters);

  std::vector<double> window;
  window.resize(n_fft);
  hanning_window(window, n_fft + 1);

  std::vector<double> pad_data;
  pad_data.resize(max_length + n_fft);
  for (size_t i = 0; i < n_fft / 2; ++i) {
    pad_data[n_fft / 2 - 1 - i] = p_data[i + 1];
  }
  for (size_t i = 0; i < max_length; ++i) {
    pad_data[n_fft / 2 + i] = p_data[i];
  }
  for (size_t i = 0; i < n_fft / 2; ++i) {
    pad_data[n_fft / 2 + max_length + i] = p_data[max_length - 2 - i];
  }

  int num_frames = 1 + (pad_data.size() - n_fft) / hop_length;
  std::vector<std::vector<double>> log_specs;
  log_specs.resize(num_frames - 1);
  for (size_t i = 0; i < num_frames - 1; ++i) {
    log_specs[i].resize(n_mels);
    log_mel_spec(pad_data, i, window, n_fft, hop_length, mel_filters, log_specs);
  }

  double log_specs_max = std::numeric_limits<float>::min();
  for (size_t i = 0; i < log_specs.size(); i++) {
    for (size_t j = 0; j < n_mels; ++j) {
      log_specs_max = std::max(log_specs_max, log_specs[i][j]);
    }
  }

  log_specs_max -= 8.0;
  for (size_t i = 0; i < log_specs.size(); i++) {
    for (size_t j = 0; j < n_mels; ++j) {
      log_specs[i][j] = std::max(log_specs_max, log_specs[i][j]);
      log_specs[i][j] = (log_specs[i][j] + 4.0) / 4.0;
    }
  }

  auto ret_value =
      runtime::NDArray::Empty({num_frames - 1, n_mels}, DataType::Float(32), DLDevice{kDLCPU, 0});
  float* p_ret = static_cast<float*>(ret_value->data);
  for (size_t i = 0; i < log_specs.size(); i++) {
    for (size_t j = 0; j < n_mels; ++j) {
      p_ret[i * n_mels + j] = (float)log_specs[i][j];
    }
  }
  return ret_value;
}

TVM_REGISTER_GLOBAL("vm.builtin.whisper_process_audio").set_body_typed(WhisperProcessAudio);

}  // namespace relax_vm
}  // namespace runtime
}  // namespace tvm
