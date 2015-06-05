/**
 * @file AKAZEFeatures.cpp
 * @brief Main class for detecting and describing binary features in an
 * accelerated nonlinear scale space
 * @date Sep 15, 2013
 * @author Pablo F. Alcantarilla, Jesus Nuevo
 */

#include "AKAZEFeatures.h"
#include "fed.h"
#include "nldiffusion_functions.h"
#include "utils.h"

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <cstring>
#include <cstdint>
#include <iostream>

// Taken from opencv2/internal.hpp: IEEE754 constants and macros
#define  CV_TOGGLE_FLT(x) ((x)^((int)(x) < 0 ? 0x7fffffff : 0))

// Namespaces
namespace cv
{
using namespace std;


/// Internal Functions
inline
void Compute_Main_Orientation(cv::KeyPoint& kpt, const TEvolutionV2& evolution_);
static
void generateDescriptorSubsampleV2(cv::Mat& sampleList, cv::Mat& comparisons, int nbits, int pattern_size, int nchannels);


/* ************************************************************************* */
/**
 * @brief AKAZEFeatures constructor with input options
 * @param options AKAZEFeatures configuration options
 * @note This constructor allocates memory for the nonlinear scale space
 */
AKAZEFeaturesV2::AKAZEFeaturesV2(const AKAZEOptionsV2& options) : options_(options) {

  cout << "AKAZEFeaturesV2 constructor called" << endl;

  reordering_ = true;

  if (options_.descriptor_size > 0 && options_.descriptor >= AKAZE::DESCRIPTOR_MLDB_UPRIGHT) {
    generateDescriptorSubsampleV2(descriptorSamples_, descriptorBits_, options_.descriptor_size,
                                options_.descriptor_pattern_size, options_.descriptor_channels);
  }

  Allocate_Memory_Evolution();
}

/* ************************************************************************* */
/**
 * @brief This method allocates the memory for the nonlinear diffusion evolution
 */
void AKAZEFeaturesV2::Allocate_Memory_Evolution(void) {

  CV_Assert(options_.img_height > 2 && options_.img_width > 2);  // The size of modgs_ must be positive

  // Allocate the dimension of the matrices for the evolution
  int level_height = options_.img_height;
  int level_width = options_.img_width;
  int power = 1;

  for (int i = 0; i < options_.omax; i++) {

    for (int j = 0; j < options_.nsublevels; j++) {
      TEvolutionV2 step;
      step.Lt.create(level_height, level_width, CV_32FC1);
      step.Ldet.create(level_height, level_width, CV_32FC1);
      step.Lsmooth.create(level_height, level_width, CV_32FC1);
      step.Lx.create(level_height, level_width, CV_32FC1);
      step.Ly.create(level_height, level_width, CV_32FC1);
      step.Lxx.create(level_height, level_width, CV_32FC1);
      step.Lxy.create(level_height, level_width, CV_32FC1);
      step.Lyy.create(level_height, level_width, CV_32FC1);
      step.esigma = options_.soffset*pow(2.f, (float)j / options_.nsublevels + i);
      step.sigma_size = fRoundV2(step.esigma * options_.derivative_factor / power);  // In fact sigma_size only depends on j
      step.etime = 0.5f*(step.esigma*step.esigma);
      step.octave = i;
      step.sublevel = j;
      step.octave_ratio = (float)power;

      // Pre-calculate the derivative kernels
      compute_scharr_derivative_kernelsV2(step.DxKx, step.DxKy, 1, 0, step.sigma_size);
      compute_scharr_derivative_kernelsV2(step.DyKx, step.DyKy, 0, 1, step.sigma_size);

      evolution_.push_back(step);
    }

    power <<= 1;
    level_height >>= 1;
    level_width >>= 1;

    // The next octave becomes too small
    if (level_width < 80 || level_height < 40) {
      options_.omax = i + 1;
      break;
    }
  }

  // Allocate memory for workspaces
  lflow_.create(options_.img_height, options_.img_width, CV_32FC1);
  lstep_.create(options_.img_height, options_.img_width, CV_32FC1);
  histgram_.resize(options_.kcontrast_nbins);
  modgs_.resize((options_.img_height - 2) * (options_.img_width - 2));  // excluding the border

  kpts_aux_.resize(evolution_.size());
  for (size_t i = 0; i < evolution_.size(); i++)
      kpts_aux_[i].reserve(1024);  // reserve 1K points' space for each evolution step

  // Allocate memory for the number of cycles and time steps
  tsteps_.resize(evolution_.size() - 1);
  for (size_t i = 1; i < evolution_.size(); i++) {
    fed_tau_by_process_timeV2(evolution_[i].etime - evolution_[i - 1].etime,
                              1, 0.25f, reordering_, tsteps_[i - 1]);
  }
}


/* ************************************************************************* */
/**
 * @brief This method creates the nonlinear scale space for a given image
 * @param img Input image for which the nonlinear scale space needs to be created
 * @return 0 if the nonlinear scale space was created successfully, -1 otherwise
 */
int AKAZEFeaturesV2::Create_Nonlinear_Scale_Space(const Mat& img)
{
  CV_Assert(evolution_.size() > 0);

  // Setup the gray-scale image
  const Mat * gray = &img;
  if (img.channels() != 1) {
    cvtColor(img, gray_, COLOR_BGR2GRAY);
    gray = & gray_;
  }

  if (gray->type() == CV_8UC1) {
    gray->convertTo(evolution_[0].Lt, CV_32F, 1/255.0);
    gray = &evolution_[0].Lt;
  }
  else if (gray->type() == CV_16UC1) {
    gray->convertTo(evolution_[0].Lt, CV_32F, 1/65535.0);
    gray = &evolution_[0].Lt;
  }
  CV_Assert(gray->type() == CV_32FC1);


  // First compute the kcontrast factor
  gaussian_2D_convolutionV2(*gray, evolution_[0].Lsmooth, 0, 0, 1.0f);
  image_derivatives_scharrV2(evolution_[0].Lsmooth, evolution_[0].Lx, 1, 0);
  image_derivatives_scharrV2(evolution_[0].Lsmooth, evolution_[0].Ly, 0, 1);
  float kcontrast = compute_k_percentileV2(evolution_[0].Lx, evolution_[0].Ly, options_.kcontrast_percentile, modgs_, histgram_);

  // Copy the original image to the first level of the evolution
  gaussian_2D_convolutionV2(*gray, evolution_[0].Lsmooth, 0, 0, options_.soffset);
  evolution_[0].Lsmooth.copyTo(evolution_[0].Lt);

  // Prepare the flow and step images
  Mat Lflow(evolution_[0].Lt.rows, evolution_[0].Lt.cols, CV_32FC1, lflow_.data);
  Mat Lstep(evolution_[0].Lt.rows, evolution_[0].Lt.cols, CV_32FC1, lstep_.data);

  // Now generate the rest of evolution levels
  for (size_t i = 1; i < evolution_.size(); i++) {

    if (evolution_[i].octave > evolution_[i - 1].octave) {
      halfsample_imageV2(evolution_[i - 1].Lt, evolution_[i].Lt);
      kcontrast = kcontrast*0.75f;

      // Resize the flow and step images to fit Lt
      Lflow = cv::Mat(evolution_[i].Lt.rows, evolution_[i].Lt.cols, CV_32FC1, lflow_.data);
      Lstep = cv::Mat(evolution_[i].Lt.rows, evolution_[i].Lt.cols, CV_32FC1, lstep_.data);
    }
    else {
      evolution_[i - 1].Lt.copyTo(evolution_[i].Lt);
    }

    // Compute the Gaussian derivatives Lx and Ly
    gaussian_2D_convolutionV2(evolution_[i].Lt, evolution_[i].Lsmooth, 0, 0, 1.0f);
    image_derivatives_scharrV2(evolution_[i].Lsmooth, evolution_[i].Lx, 1, 0);
    image_derivatives_scharrV2(evolution_[i].Lsmooth, evolution_[i].Ly, 0, 1);

    // Compute the conductivity equation
    switch (options_.diffusivity) {
      case KAZE::DIFF_PM_G1:
        pm_g1V2(evolution_[i].Lx, evolution_[i].Ly, Lflow, kcontrast);
      break;
      case KAZE::DIFF_PM_G2:
        pm_g2V2(evolution_[i].Lx, evolution_[i].Ly, Lflow, kcontrast);
      break;
      case KAZE::DIFF_WEICKERT:
        weickert_diffusivityV2(evolution_[i].Lx, evolution_[i].Ly, Lflow, kcontrast);
      break;
      case KAZE::DIFF_CHARBONNIER:
        charbonnier_diffusivityV2(evolution_[i].Lx, evolution_[i].Ly, Lflow, kcontrast);
      break;
      default:
        CV_Error(options_.diffusivity, "Diffusivity is not supported");
      break;
    }

    const int total = Lstep.rows * Lstep.cols;
    float * lt = evolution_[i].Lt.ptr<float>(0);
    float * lstep = Lstep.ptr<float>(0);
    std::vector<float> & tsteps = tsteps_[i - 1];

    // Perform FED n inner steps
    for (int j = 0; j < tsteps.size(); j++) {
      nld_step_scalarV2(evolution_[i].Lt, Lflow, Lstep);

      const float step_size = tsteps[j];
      for (int k = 0; k < total; k++)
        lt[k] += lstep[k] * 0.5f * step_size;
    }
  }

  return 0;
}

/* ************************************************************************* */
/**
 * @brief This method selects interesting keypoints through the nonlinear scale space
 * @param kpts Vector of detected keypoints
 */
void AKAZEFeaturesV2::Feature_Detection(std::vector<KeyPoint>& kpts)
{
  Compute_Determinant_Hessian_Response();
  Find_Scale_Space_Extrema(kpts_aux_);
  Do_Subpixel_Refinement(kpts_aux_, kpts);
}

/* ************************************************************************* */
class MultiscaleDerivativesAKAZEInvokerV2 : public ParallelLoopBody
{
public:
    explicit MultiscaleDerivativesAKAZEInvokerV2(std::vector<TEvolutionV2>& ev, const AKAZEOptionsV2& opt)
    : evolution_(&ev)
    , options_(opt)
  {
  }

  void operator()(const Range& range) const
  {
    std::vector<TEvolutionV2>& evolution = *evolution_;

    for (int i = range.start; i < range.end; i++)
    {
      sepFilter2D(evolution[i].Lsmooth, evolution[i].Lx, CV_32F, evolution[i].DxKx, evolution[i].DxKy);
      sepFilter2D(evolution[i].Lsmooth, evolution[i].Ly, CV_32F, evolution[i].DyKx, evolution[i].DyKy);
      sepFilter2D(evolution[i].Lx, evolution[i].Lxx, CV_32F, evolution[i].DxKx, evolution[i].DxKy);
      sepFilter2D(evolution[i].Ly, evolution[i].Lyy, CV_32F, evolution[i].DyKx, evolution[i].DyKy);
      sepFilter2D(evolution[i].Lx, evolution[i].Lxy, CV_32F, evolution[i].DyKx, evolution[i].DyKy);

      evolution[i].Lx = evolution[i].Lx*(evolution[i].sigma_size);
      evolution[i].Ly = evolution[i].Ly*(evolution[i].sigma_size);
      evolution[i].Lxx = evolution[i].Lxx*(evolution[i].sigma_size * evolution[i].sigma_size);
      evolution[i].Lxy = evolution[i].Lxy*(evolution[i].sigma_size * evolution[i].sigma_size);
      evolution[i].Lyy = evolution[i].Lyy*(evolution[i].sigma_size * evolution[i].sigma_size);
    }
  }

private:
  std::vector<TEvolutionV2>*  evolution_;
  AKAZEOptionsV2              options_;
};

/* ************************************************************************* */
/**
 * @brief This method computes the multiscale derivatives for the nonlinear scale space
 */
void AKAZEFeaturesV2::Compute_Multiscale_Derivatives(void)
{
  parallel_for_(Range(0, (int)evolution_.size()),
                                        MultiscaleDerivativesAKAZEInvokerV2(evolution_, options_));
}

/* ************************************************************************* */
/**
 * @brief This method computes the feature detector response for the nonlinear scale space
 * @note We use the Hessian determinant as the feature detector response
 */
void AKAZEFeaturesV2::Compute_Determinant_Hessian_Response(void) {

  // Firstly compute the multiscale derivatives
  Compute_Multiscale_Derivatives();

  for (size_t i = 0; i < evolution_.size(); i++)
  {
    const int total = evolution_[i].Ldet.rows * evolution_[i].Ldet.cols;
    const float * lxx = evolution_[i].Lxx.ptr<float>(0);
    const float * lxy = evolution_[i].Lxy.ptr<float>(0);
    const float * lyy = evolution_[i].Lyy.ptr<float>(0);
    float * ldet = evolution_[i].Ldet.ptr<float>(0);

    // Compute Ldet by Lxx.mul(Lyy) - Lxy.mul(Lxy)
    for (int j = 0; j < total; j++) {
      ldet[j] = lxx[j]*lyy[j] - lxy[j]*lxy[j];
    }
  }
}

/* ************************************************************************* */
/**
 * @brief This method searches v for a neighbor point of the point candidate p
 * @param p The keypoint candidate to search a neighbor
 * @param v The vector to store the points to be searched
 * @param offset The starting location in the vector v to be searched at
 * @param idx The index of the vector v if a neighbor is found
 * @return true if a neighbor point is found; false otherwise
 */
inline
bool find_neighbor_point(const KeyPoint &p, const vector<KeyPoint> &v, const int offset, int &idx)
{
    const int sz = (int)v.size();

    for (int i = offset; i < sz; i++) {

        if (v[i].class_id == -1) // Skip a deleted point
            continue;

        float dx = p.pt.x - v[i].pt.x;
        float dy = p.pt.y - v[i].pt.y;
        if (dx * dx + dy * dy <= p.size * p.size) {
            idx = i;
            return true;
        }
    }

    return false;
}

/* ************************************************************************* */
/**
 * @brief This method finds extrema in the nonlinear scale space
 * @param kpts_aux Output vectors of detected keypoints; one vector for each evolution level
 */
void AKAZEFeaturesV2::Find_Scale_Space_Extrema(std::vector<vector<KeyPoint>>& kpts_aux)
{
  // Clear the workspace to hold the detected keypoint candidates
  for (size_t i = 0; i < kpts_aux_.size(); i++)
    kpts_aux_[i].clear();

  // Set maximum size
  float smax = 0.0;
  if (options_.descriptor == AKAZE::DESCRIPTOR_MLDB_UPRIGHT || options_.descriptor == AKAZE::DESCRIPTOR_MLDB) {
    smax = 10.0f*sqrtf(2.0f);
  }
  else if (options_.descriptor == AKAZE::DESCRIPTOR_KAZE_UPRIGHT || options_.descriptor == AKAZE::DESCRIPTOR_KAZE) {
    smax = 12.0f*sqrtf(2.0f);
  }

  for (int i = 0; i < (int)evolution_.size(); i++) {
    const TEvolutionV2 &step = evolution_[i];

    // Descriptors of the points on the border cannot be computed; exclude them first
    const int border = fRoundV2(smax * step.sigma_size) + 1;

    const float * prev = step.Ldet.ptr<float>(border - 1);
    const float * curr = step.Ldet.ptr<float>(border    );
    const float * next = step.Ldet.ptr<float>(border + 1);

    for (int y = border; y < step.Ldet.rows - border; y++) {
      for (int x = border; x < step.Ldet.cols - border; x++) {

        const float value = curr[x];

        // Filter the points with the detector threshold
        if (value <= options_.dthreshold || value < options_.min_dthreshold)
          continue;
        if (value <= curr[x-1] || value <= curr[x+1])
          continue;
        if (value <= prev[x-1] || value <= prev[x  ] || value <= prev[x+1])
          continue;
        if (value <= next[x-1] || value <= next[x  ] || value <= next[x+1])
          continue;

        KeyPoint point( /* x */ static_cast<float>(x * step.octave_ratio),
                        /* y */ static_cast<float>(y * step.octave_ratio),
                        /* size */ step.esigma * options_.derivative_factor,
                        /* angle */ -1,
                        /* response */ value,
                        /* octave */ step.octave,
                        /* class_id */ i);

        int idx = 0;

        // Compare response with the same scale
        if (find_neighbor_point(point, kpts_aux[i], 0, idx)) {
          if (point.response > kpts_aux[i][idx].response)
            kpts_aux[i][idx] = point;  // Replace the old point
          continue;
        }

        // Compare response with the lower scale
        if (i > 0 && find_neighbor_point(point, kpts_aux[i - 1], 0, idx)) {
          if (point.response > kpts_aux[i - 1][idx].response) {
            kpts_aux[i - 1][idx].class_id = -1;  // Mark it as deleted
            kpts_aux[i].push_back(point);  // Insert the new point to the right layer
          }
          continue;
        }

        kpts_aux[i].push_back(point);  // A good keypoint candidate is found
      }
      prev = curr;
      curr = next;
      next += step.Ldet.cols;
    }
  }

  // Now filter points with the upper scale level
  for (int i = 0; i < (int)kpts_aux.size() - 1; i++) {
    for (int j = 0; j < (int)kpts_aux[i].size(); j++) {

      KeyPoint& pt = kpts_aux[i][j];

      if (pt.class_id == -1) // Skip a deleted point
          continue;

      int idx = 0;
      if (find_neighbor_point(pt, kpts_aux[i + 1], j + 1, idx)) {
        if (pt.response < kpts_aux[i + 1][idx].response)
          pt.class_id = -1; // Non-extremum; mark this point deleted
      }
    }
  }
}

/* ************************************************************************* */
/**
 * @brief This method performs subpixel refinement of the detected keypoints
 * @param kpts_aux Input vectors of detected keypoints, sorted by evolution levels
 * @param kpts Output vector of the final refined keypoints
 */
void AKAZEFeaturesV2::Do_Subpixel_Refinement(std::vector<std::vector<KeyPoint>>& kpts_aux, std::vector<KeyPoint>& kpts)
{
  // Clear the keypoint vector
  kpts.clear();

  for (int i = 0; i < (int)kpts_aux.size(); i++) {
    const float * const ldet = evolution_[i].Ldet.ptr<float>(0);
    const float ratio = evolution_[i].octave_ratio;
    const int cols = evolution_[i].Ldet.cols;

    for (int j = 0; j < (int)kpts_aux[i].size(); j++) {

      KeyPoint & kp = kpts_aux[i][j];

      if (kp.class_id == -1)
        continue; // Skip a deleted keypoint

      int x = (int)(kp.pt.x / ratio);
      int y = (int)(kp.pt.y / ratio);

      // Compute the gradient
      float Dx = 0.5f * (ldet[ y     *cols + x + 1] - ldet[ y     *cols + x - 1]);
      float Dy = 0.5f * (ldet[(y + 1)*cols + x    ] - ldet[(y - 1)*cols + x    ]);

      // Compute the Hessian
      float Dxx = ldet[ y     *cols + x + 1] + ldet[ y     *cols + x - 1] - 2.0f * ldet[y*cols + x];
      float Dyy = ldet[(y + 1)*cols + x    ] + ldet[(y - 1)*cols + x    ] - 2.0f * ldet[y*cols + x];
      float Dxy = 0.25f * (ldet[(y + 1)*cols + x + 1] + ldet[(y - 1)*cols + x - 1]
                         - ldet[(y - 1)*cols + x + 1] - ldet[(y + 1)*cols + x - 1]);

      // Solve the linear system
      Matx22f A{ Dxx, Dxy,
                 Dxy, Dyy };
      Vec2f   b{ -Dx, -Dy };
      Vec2f   dst{ 0.0f, 0.0f };
      solve(A, b, dst, DECOMP_LU);

      float dx = dst(0);
      float dy = dst(1);

      if (fabs(dx) > 1.0f || fabs(dy) > 1.0f)
        continue; // Ignore the point that is not stable

      // Refine the coordinates
      kp.pt.x += dx * ratio;
      kp.pt.y += dy * ratio;

      kp.angle = 0.0;
      kp.size *= 2.0f; // In OpenCV the size of a keypoint is the diameter

      // Push the refined keypoint to the final storage
      kpts.push_back(kp);
    }
  }
}

/* ************************************************************************* */

class SURF_Descriptor_Upright_64_InvokerV2 : public ParallelLoopBody
{
public:
  SURF_Descriptor_Upright_64_InvokerV2(std::vector<KeyPoint>& kpts, Mat& desc, const std::vector<TEvolutionV2>& evolution)
    : keypoints_(kpts)
    , descriptors_(desc)
    , evolution_(evolution)
  {
  }

  void operator() (const Range& range) const
  {
    for (int i = range.start; i < range.end; i++)
    {
      Get_SURF_Descriptor_Upright_64(keypoints_[i], descriptors_.ptr<float>(i));
    }
  }

  void Get_SURF_Descriptor_Upright_64(const KeyPoint& kpt, float* desc) const;

private:
  std::vector<KeyPoint> & keypoints_;
  Mat                   & descriptors_;
  const std::vector<TEvolutionV2> & evolution_;
};

class SURF_Descriptor_64_InvokerV2 : public ParallelLoopBody
{
public:
  SURF_Descriptor_64_InvokerV2(std::vector<KeyPoint>& kpts, Mat& desc, const std::vector<TEvolutionV2>& evolution)
    : keypoints_(kpts)
    , descriptors_(desc)
    , evolution_(evolution)
  {
  }

  void operator()(const Range& range) const
  {
    for (int i = range.start; i < range.end; i++)
    {
      KeyPoint &kp{ keypoints_[i] };
      Compute_Main_Orientation(kp, evolution_[kp.class_id]);
      Get_SURF_Descriptor_64(kp, descriptors_.ptr<float>(i));
    }
  }

  void Get_SURF_Descriptor_64(const KeyPoint& kpt, float* desc) const;

private:
  std::vector<KeyPoint> & keypoints_;
  Mat                   & descriptors_;
  const std::vector<TEvolutionV2> & evolution_;
};

class MSURF_Upright_Descriptor_64_InvokerV2 : public ParallelLoopBody
{
public:
  MSURF_Upright_Descriptor_64_InvokerV2(std::vector<KeyPoint>& kpts, Mat& desc, const std::vector<TEvolutionV2>& evolution)
    : keypoints_(kpts)
    , descriptors_(desc)
    , evolution_(evolution)
  {
  }

  void operator()(const Range& range) const
  {
    for (int i = range.start; i < range.end; i++)
    {
      Get_MSURF_Upright_Descriptor_64(keypoints_[i], descriptors_.ptr<float>(i));
    }
  }

  void Get_MSURF_Upright_Descriptor_64(const KeyPoint& kpt, float* desc) const;

private:
  std::vector<KeyPoint> & keypoints_;
  Mat                   & descriptors_;
  const std::vector<TEvolutionV2> & evolution_;
};

class MSURF_Descriptor_64_InvokerV2 : public ParallelLoopBody
{
public:
  MSURF_Descriptor_64_InvokerV2(std::vector<KeyPoint>& kpts, Mat& desc, const std::vector<TEvolutionV2>& evolution)
    : keypoints_(kpts)
    , descriptors_(desc)
    , evolution_(evolution)
  {
  }

  void operator() (const Range& range) const
  {
    for (int i = range.start; i < range.end; i++)
    {
      Compute_Main_Orientation(keypoints_[i], evolution_[keypoints_[i].class_id]);
      Get_MSURF_Descriptor_64(keypoints_[i], descriptors_.ptr<float>(i));
    }
  }

  void Get_MSURF_Descriptor_64(const KeyPoint& kpt, float* desc) const;

private:
  std::vector<KeyPoint> & keypoints_;
  Mat                   & descriptors_;
  const std::vector<TEvolutionV2> & evolution_;
};

class Upright_MLDB_Full_Descriptor_InvokerV2 : public ParallelLoopBody
{
public:
  Upright_MLDB_Full_Descriptor_InvokerV2(std::vector<KeyPoint>& kpts,
                                         Mat& desc,
                                         const std::vector<TEvolutionV2>& evolution,
                                         const AKAZEOptionsV2& options)
    : keypoints_(kpts)
    , descriptors_(desc)
    , evolution_(evolution)
    , options_(options)
  {
  }

  void operator() (const Range& range) const
  {
    for (int i = range.start; i < range.end; i++)
    {
      Get_Upright_MLDB_Full_Descriptor(keypoints_[i], descriptors_.ptr<unsigned char>(i));
    }
  }

  void Get_Upright_MLDB_Full_Descriptor(const KeyPoint& kpt, unsigned char* desc) const;

private:
  std::vector<KeyPoint> & keypoints_;
  Mat                   & descriptors_;
  const std::vector<TEvolutionV2> & evolution_;
  const AKAZEOptionsV2            & options_;
};

class Upright_MLDB_Descriptor_Subset_InvokerV2 : public ParallelLoopBody
{
public:
  Upright_MLDB_Descriptor_Subset_InvokerV2(std::vector<KeyPoint>& kpts,
                                           Mat& desc,
                                           const std::vector<TEvolutionV2>& evolution,
                                           const AKAZEOptionsV2& options,
                                           const Mat & descriptorSamples,
                                           const Mat & descriptorBits)
    : keypoints_(kpts)
    , descriptors_(desc)
    , evolution_(evolution)
    , options_(options)
    , descriptorSamples_(descriptorSamples)
    , descriptorBits_(descriptorBits)
  {
  }

  void operator() (const Range& range) const
  {
    for (int i = range.start; i < range.end; i++)
    {
      Get_Upright_MLDB_Descriptor_Subset(keypoints_[i], descriptors_.ptr<unsigned char>(i));
    }
  }

  void Get_Upright_MLDB_Descriptor_Subset(const KeyPoint& kpt, unsigned char* desc) const;

private:
  std::vector<KeyPoint> & keypoints_;
  Mat                   & descriptors_;
  const std::vector<TEvolutionV2> & evolution_;
  const AKAZEOptionsV2            & options_;

  const Mat & descriptorSamples_;  // List of positions in the grids to sample LDB bits from.
  const Mat & descriptorBits_;
};

class MLDB_Full_Descriptor_InvokerV2 : public ParallelLoopBody
{
public:
  MLDB_Full_Descriptor_InvokerV2(std::vector<KeyPoint>& kpts,
                                 Mat& desc,
                                 const std::vector<TEvolutionV2>& evolution,
                                 const AKAZEOptionsV2& options)
    : keypoints_(kpts)
    , descriptors_(desc)
    , evolution_(evolution)
    , options_(options)
  {
  }

  void operator() (const Range& range) const
  {
    for (int i = range.start; i < range.end; i++)
    {
      Compute_Main_Orientation(keypoints_[i], evolution_[keypoints_[i].class_id]);
      Get_MLDB_Full_Descriptor(keypoints_[i], descriptors_.ptr<unsigned char>(i));
    }
  }

  void Get_MLDB_Full_Descriptor(const KeyPoint& kpt, unsigned char* desc) const;
  void MLDB_Fill_Values(float* values, int sample_step, int level,
                        float xf, float yf, float co, float si, float scale) const;
  void MLDB_Binary_Comparisons(float* values, unsigned char* desc,
                               int count, int& dpos) const;

private:
  std::vector<KeyPoint> & keypoints_;
  Mat                   & descriptors_;
  const std::vector<TEvolutionV2> & evolution_;
  const AKAZEOptionsV2            & options_;
};

class MLDB_Descriptor_Subset_InvokerV2 : public ParallelLoopBody
{
public:
  MLDB_Descriptor_Subset_InvokerV2(std::vector<KeyPoint>& kpts,
                                 Mat& desc,
                                 const std::vector<TEvolutionV2>& evolution,
                                 const AKAZEOptionsV2& options,
                                 const Mat& descriptorSamples,
                                 const Mat& descriptorBits)
    : keypoints_(kpts)
    , descriptors_(desc)
    , evolution_(evolution)
    , options_(options)
    , descriptorSamples_(descriptorSamples)
    , descriptorBits_(descriptorBits)
  {
  }

  void operator() (const Range& range) const
  {
    for (int i = range.start; i < range.end; i++)
    {
      Compute_Main_Orientation(keypoints_[i], evolution_[keypoints_[i].class_id]);
      Get_MLDB_Descriptor_Subset(keypoints_[i], descriptors_.ptr<unsigned char>(i));
    }
  }

  void Get_MLDB_Descriptor_Subset(const KeyPoint& kpt, unsigned char* desc) const;

private:
  std::vector<KeyPoint> & keypoints_;
  Mat                   & descriptors_;
  const std::vector<TEvolutionV2> & evolution_;
  const AKAZEOptionsV2            & options_;

  const Mat& descriptorSamples_;  // List of positions in the grids to sample LDB bits from.
  const Mat& descriptorBits_;
};

/**
 * @brief This method  computes the set of descriptors through the nonlinear scale space
 * @param kpts Vector of detected keypoints
 * @param desc Matrix to store the descriptors
 */
void AKAZEFeaturesV2::Compute_Descriptors(std::vector<KeyPoint>& kpts, Mat& desc)
{
  for(size_t i = 0; i < kpts.size(); i++)
  {
      CV_Assert(0 <= kpts[i].class_id && kpts[i].class_id < static_cast<int>(evolution_.size()));
  }

  // Allocate memory for the matrix with the descriptors
  if (options_.descriptor < AKAZE::DESCRIPTOR_MLDB_UPRIGHT) {
    desc.create((int)kpts.size(), 64, CV_32FC1);
  }
  else {
    // We use the full length binary descriptor -> 486 bits
    if (options_.descriptor_size == 0) {
      int t = (6 + 36 + 120)*options_.descriptor_channels;
      desc.create((int)kpts.size(), (int)ceil(t / 8.), CV_8UC1);
    }
    else {
      // We use the random bit selection length binary descriptor
      desc.create((int)kpts.size(), (int)ceil(options_.descriptor_size / 8.), CV_8UC1);
    }
  }

  switch (options_.descriptor)
  {
    case AKAZE::DESCRIPTOR_KAZE_UPRIGHT: // Upright descriptors, not invariant to rotation
    {
      parallel_for_(Range(0, (int)kpts.size()), MSURF_Upright_Descriptor_64_InvokerV2(kpts, desc, evolution_));
    }
    break;
    case AKAZE::DESCRIPTOR_KAZE:
    {
      parallel_for_(Range(0, (int)kpts.size()), MSURF_Descriptor_64_InvokerV2(kpts, desc, evolution_));
    }
    break;
    case AKAZE::DESCRIPTOR_MLDB_UPRIGHT: // Upright descriptors, not invariant to rotation
    {
      if (options_.descriptor_size == 0)
        parallel_for_(Range(0, (int)kpts.size()), Upright_MLDB_Full_Descriptor_InvokerV2(kpts, desc, evolution_, options_));
      else
        parallel_for_(Range(0, (int)kpts.size()), Upright_MLDB_Descriptor_Subset_InvokerV2(kpts, desc, evolution_, options_, descriptorSamples_, descriptorBits_));
    }
    break;
    case AKAZE::DESCRIPTOR_MLDB:
    {
      if (options_.descriptor_size == 0)
        parallel_for_(Range(0, (int)kpts.size()), MLDB_Full_Descriptor_InvokerV2(kpts, desc, evolution_, options_));
      else
        parallel_for_(Range(0, (int)kpts.size()), MLDB_Descriptor_Subset_InvokerV2(kpts, desc, evolution_, options_, descriptorSamples_, descriptorBits_));
    }
    break;
  }
}

/* ************************************************************************* */
/**
 * @brief This function samples the derivative responses Lx and Ly for the points
 * within the radius of 6*scale from (x0, y0), then multiply 2D Gaussian weight
 * @param Lx Horizontal derivative
 * @param Ly Vertical derivative
 * @param x0 X-coordinate of the center point
 * @param y0 Y-coordinate of the center point
 * @param scale The sampling step
 * @param resX Output array of the weighted horizontal derivative responses
 * @param resY Output array of the weighted vertical derivative responses
 */
static inline
void Sample_Derivative_Response_Radius6(const Mat &Lx, const Mat &Ly,
                                  const int x0, const int y0, const int scale,
                                  float *resX, float *resY)
{
    /* ************************************************************************* */
    /// Lookup table for 2d gaussian (sigma = 2.5) where (0,0) is top left and (6,6) is bottom right
    static const float gauss25[7][7] =
    {
        { 0.02546481f, 0.02350698f, 0.01849125f, 0.01239505f, 0.00708017f, 0.00344629f, 0.00142946f },
        { 0.02350698f, 0.02169968f, 0.01706957f, 0.01144208f, 0.00653582f, 0.00318132f, 0.00131956f },
        { 0.01849125f, 0.01706957f, 0.01342740f, 0.00900066f, 0.00514126f, 0.00250252f, 0.00103800f },
        { 0.01239505f, 0.01144208f, 0.00900066f, 0.00603332f, 0.00344629f, 0.00167749f, 0.00069579f },
        { 0.00708017f, 0.00653582f, 0.00514126f, 0.00344629f, 0.00196855f, 0.00095820f, 0.00039744f },
        { 0.00344629f, 0.00318132f, 0.00250252f, 0.00167749f, 0.00095820f, 0.00046640f, 0.00019346f },
        { 0.00142946f, 0.00131956f, 0.00103800f, 0.00069579f, 0.00039744f, 0.00019346f, 0.00008024f }
    };
    static const int id[] = { 6, 5, 4, 3, 2, 1, 0, 1, 2, 3, 4, 5, 6 };

    const int sz = 109;
    static float gweight[sz];
    static int8_t xidx[sz];
    static int8_t yidx[sz];

    static bool initialized = false;


  if (!initialized) {
    int k = 0;

    // Generate the indices
    for (int i = -6; i <= 6; ++i) {
      for (int j = -6; j <= 6; ++j) {
        if (i*i + j*j < 36) {
          gweight[k] = gauss25[id[i + 6]][id[j + 6]];
          yidx[k] = i;
          xidx[k] = j;
          ++k;
        }
      }
    }
    CV_DbgAssert(k == sz);

    initialized = true;
  }

  const float * lx = Lx.ptr<float>(0);
  const float * ly = Ly.ptr<float>(0);
  int cols = Lx.cols;

  for (int i = 0; i < sz; i++) {
    int j = (y0 + yidx[i] * scale) * cols + (x0 + xidx[i] * scale);

    resX[i] = gweight[i] * lx[j];
    resY[i] = gweight[i] * ly[j];
  }
}

/* ************************************************************************* */
/**
 * @brief This function sorts a[] by quantized float values
 * @param a[] Input floating point array to sort
 * @param n The length of a[]
 * @param quantum The interval to convert a[]'s float values to integers
 * @param max The upper bound of a[]'s values
 * @param idx[] Output array of the indices: a[idx[i]] is a sorted array
 * @param cum[] Output array of the starting indices of quantized floats
 * @note The values of a[] in [k*quantum, (k + 1)*quantum) is labeled by
 * the integer k, which is calculated by floor(a[i]/quantum).  After sorting,
 * the values from a[idx[cum[k]]] to a[idx[cum[k+1]-1]] are all labeled by k.
 * This sorting is unstable in order to reduce the computation.
 */
static inline
void quantized_counting_sort(const float a[], const int n,
                             const float quantum, const float max,
                             uint8_t idx[], uint8_t cum[])
{
  const int nkeys = (int)(max / quantum) + 1;

  memset(cum, 0, nkeys + 1);

  // Count up the quantized values
  for (int i = 0; i < n; i++)
    cum[(int)(a[i] / quantum)]++;

  // Compute the inclusive prefix sum i.e. the end indices; cum[nkeys] is the total
  for (int i = 1; i <= nkeys; i++)
    cum[i] += cum[i - 1];

  // Generate the sorted indices; cum[] becomes the exclusive prefix sum i.e. the start indices of keys
  for (int i = 0; i < n; i++)
    idx[--cum[(int)(a[i] / quantum)]] = i;
}


/* ************************************************************************* */
/**
 * @brief This function computes the main orientation for a given keypoint
 * @param kpt Input keypoint
 * @note The orientation is computed using a similar approach as described in the
 * original SURF method. See Bay et al., Speeded Up Robust Features, ECCV 2006
 */
inline
void Compute_Main_Orientation(KeyPoint& kpt, const TEvolutionV2& e)
{
  // Get the information from the keypoint
  int scale = fRoundV2(0.5f * kpt.size / e.octave_ratio);
  int x0 = fRoundV2(kpt.pt.x / e.octave_ratio);
  int y0 = fRoundV2(kpt.pt.y / e.octave_ratio);

  // Sample derivatives responses for the points within radius of 6*scale
  const int ang_size = 109;
  float resX[ang_size], resY[ang_size];
  Sample_Derivative_Response_Radius6(e.Lx, e.Ly, x0, y0, scale, resX, resY);

  // Compute the angle of each gradient vector
  float Ang[ang_size];
  fastAtan2(resY, resX, Ang, ang_size, false);

  // Sort by the angles; angles are labeled by slices of 0.15 radian
  const int slices = (int)(2.0 * CV_PI / 0.15f) + 1;  /* i.e. 42 */
  uint8_t slice[slices + 1];
  uint8_t sorted_idx[ang_size];
  quantized_counting_sort(Ang, ang_size, 0.15f, (float)(2.0 * CV_PI), sorted_idx, slice);

  // Find the main angle by sliding a 7-slice-size window (1.05 = PI/3 approx) around the keypoint
  const int win = 7;

  float maxX = 0.0f, maxY = 0.0f;
  for (int i = slice[0]; i < slice[win]; i++) {
    maxX += resX[sorted_idx[i]];
    maxY += resY[sorted_idx[i]];
  }
  float maxNorm = maxX * maxX + maxY * maxY;

  for (int sn = 1; sn <= slices - win; sn++) {

    if (slice[sn] == slice[sn - 1] && slice[sn + win] == slice[sn + win - 1])
      continue;  // The contents of the window didn't change; don't repeat the computation

    float sumX = 0.0f, sumY = 0.0f;
    for (int i = slice[sn]; i < slice[sn + win]; i++) {
      sumX += resX[sorted_idx[i]];
      sumY += resY[sorted_idx[i]];
    }

    float norm = sumX * sumX + sumY * sumY;
    if (norm > maxNorm)
        maxNorm = norm, maxX = sumX, maxY = sumY;  // Found bigger one; update
  }

  for (int sn = slices - win + 1; sn < slices; sn++) {
    int remain = sn + win - slices;

    if (slice[sn] == slice[sn - 1] && slice[remain] == slice[remain - 1])
      continue;

    float sumX = 0.0f, sumY = 0.0f;
    for (int i = slice[sn]; i < slice[slices]; i++) {
      sumX += resX[sorted_idx[i]];
      sumY += resY[sorted_idx[i]];
    }
    for (int i = slice[0]; i < slice[remain]; i++) {
      sumX += resX[sorted_idx[i]];
      sumY += resY[sorted_idx[i]];
    }

    float norm = sumX * sumX + sumY * sumY;
    if (norm > maxNorm)
        maxNorm = norm, maxX = sumX, maxY = sumY;
  }

  // Store the final result
  kpt.angle = getAngleV2(maxX, maxY);
}

/* ************************************************************************* */
/**
 * @brief This method computes the upright descriptor (not rotation invariant) of
 * the provided keypoint
 * @param kpt Input keypoint
 * @param desc Descriptor vector
 * @note Rectangular grid of 24 s x 24 s. Descriptor Length 64. The descriptor is inspired
 * from Agrawal et al., CenSurE: Center Surround Extremas for Realtime Feature Detection and Matching,
 * ECCV 2008
 */
void MSURF_Upright_Descriptor_64_InvokerV2::Get_MSURF_Upright_Descriptor_64(const KeyPoint& kpt, float *desc) const {

  float dx = 0.0, dy = 0.0, mdx = 0.0, mdy = 0.0, gauss_s1 = 0.0, gauss_s2 = 0.0;
  float rx = 0.0, ry = 0.0, len = 0.0, xf = 0.0, yf = 0.0, ys = 0.0, xs = 0.0;
  float sample_x = 0.0, sample_y = 0.0;
  int x1 = 0, y1 = 0, sample_step = 0, pattern_size = 0;
  int x2 = 0, y2 = 0, kx = 0, ky = 0, i = 0, j = 0, dcount = 0;
  float fx = 0.0, fy = 0.0, ratio = 0.0, res1 = 0.0, res2 = 0.0, res3 = 0.0, res4 = 0.0;
  int scale = 0, dsize = 0, level = 0;

  // Subregion centers for the 4x4 gaussian weighting
  float cx = -0.5f, cy = 0.5f;

  // Set the descriptor size and the sample and pattern sizes
  dsize = 64;
  sample_step = 5;
  pattern_size = 12;

  // Get the information from the keypoint
  level = kpt.class_id;
  ratio = evolution_[level].octave_ratio;
  scale = fRoundV2(0.5f*kpt.size / ratio);
  yf = kpt.pt.y / ratio;
  xf = kpt.pt.x / ratio;

  i = -8;

  // Calculate descriptor for this interest point
  // Area of size 24 s x 24 s
  while (i < pattern_size) {
    j = -8;
    i = i - 4;

    cx += 1.0f;
    cy = -0.5f;

    while (j < pattern_size) {
      dx = dy = mdx = mdy = 0.0;
      cy += 1.0f;
      j = j - 4;

      ky = i + sample_step;
      kx = j + sample_step;

      ys = yf + (ky*scale);
      xs = xf + (kx*scale);

      for (int k = i; k < i + 9; k++) {
        for (int l = j; l < j + 9; l++) {
          sample_y = k*scale + yf;
          sample_x = l*scale + xf;

          //Get the gaussian weighted x and y responses
          gauss_s1 = gaussianV2(xs - sample_x, ys - sample_y, 2.50f*scale);

          y1 = (int)(sample_y - .5);
          x1 = (int)(sample_x - .5);

          y2 = (int)(sample_y + .5);
          x2 = (int)(sample_x + .5);

          fx = sample_x - x1;
          fy = sample_y - y1;

          res1 = *(evolution_[level].Lx.ptr<float>(y1)+x1);
          res2 = *(evolution_[level].Lx.ptr<float>(y1)+x2);
          res3 = *(evolution_[level].Lx.ptr<float>(y2)+x1);
          res4 = *(evolution_[level].Lx.ptr<float>(y2)+x2);
          rx = (1.0f - fx)*(1.0f - fy)*res1 + fx*(1.0f - fy)*res2 + (1.0f - fx)*fy*res3 + fx*fy*res4;

          res1 = *(evolution_[level].Ly.ptr<float>(y1)+x1);
          res2 = *(evolution_[level].Ly.ptr<float>(y1)+x2);
          res3 = *(evolution_[level].Ly.ptr<float>(y2)+x1);
          res4 = *(evolution_[level].Ly.ptr<float>(y2)+x2);
          ry = (1.0f - fx)*(1.0f - fy)*res1 + fx*(1.0f - fy)*res2 + (1.0f - fx)*fy*res3 + fx*fy*res4;

          rx = gauss_s1*rx;
          ry = gauss_s1*ry;

          // Sum the derivatives to the cumulative descriptor
          dx += rx;
          dy += ry;
          mdx += fabs(rx);
          mdy += fabs(ry);
        }
      }

      // Add the values to the descriptor vector
      gauss_s2 = gaussianV2(cx - 2.0f, cy - 2.0f, 1.5f);

      desc[dcount++] = dx*gauss_s2;
      desc[dcount++] = dy*gauss_s2;
      desc[dcount++] = mdx*gauss_s2;
      desc[dcount++] = mdy*gauss_s2;

      len += (dx*dx + dy*dy + mdx*mdx + mdy*mdy)*gauss_s2*gauss_s2;

      j += 9;
    }

    i += 9;
  }

  // convert to unit vector
  len = sqrt(len);

  for (i = 0; i < dsize; i++) {
    desc[i] /= len;
  }
}

/* ************************************************************************* */
/**
 * @brief This method computes the descriptor of the provided keypoint given the
 * main orientation of the keypoint
 * @param kpt Input keypoint
 * @param desc Descriptor vector
 * @note Rectangular grid of 24 s x 24 s. Descriptor Length 64. The descriptor is inspired
 * from Agrawal et al., CenSurE: Center Surround Extremas for Realtime Feature Detection and Matching,
 * ECCV 2008
 */
void MSURF_Descriptor_64_InvokerV2::Get_MSURF_Descriptor_64(const KeyPoint& kpt, float *desc) const {

  float dx = 0.0, dy = 0.0, mdx = 0.0, mdy = 0.0, gauss_s1 = 0.0, gauss_s2 = 0.0;
  float rx = 0.0, ry = 0.0, rrx = 0.0, rry = 0.0, len = 0.0, xf = 0.0, yf = 0.0, ys = 0.0, xs = 0.0;
  float sample_x = 0.0, sample_y = 0.0, co = 0.0, si = 0.0, angle = 0.0;
  float fx = 0.0, fy = 0.0, ratio = 0.0, res1 = 0.0, res2 = 0.0, res3 = 0.0, res4 = 0.0;
  int x1 = 0, y1 = 0, x2 = 0, y2 = 0, sample_step = 0, pattern_size = 0;
  int kx = 0, ky = 0, i = 0, j = 0, dcount = 0;
  int scale = 0, dsize = 0, level = 0;

  // Subregion centers for the 4x4 gaussian weighting
  float cx = -0.5f, cy = 0.5f;

  // Set the descriptor size and the sample and pattern sizes
  dsize = 64;
  sample_step = 5;
  pattern_size = 12;

  // Get the information from the keypoint
  level = kpt.class_id;
  ratio = evolution_[level].octave_ratio;
  scale = fRoundV2(0.5f*kpt.size / ratio);
  angle = kpt.angle;
  yf = kpt.pt.y / ratio;
  xf = kpt.pt.x / ratio;
  co = cos(angle);
  si = sin(angle);

  i = -8;

  // Calculate descriptor for this interest point
  // Area of size 24 s x 24 s
  while (i < pattern_size) {
    j = -8;
    i = i - 4;

    cx += 1.0f;
    cy = -0.5f;

    while (j < pattern_size) {
      dx = dy = mdx = mdy = 0.0;
      cy += 1.0f;
      j = j - 4;

      ky = i + sample_step;
      kx = j + sample_step;

      xs = xf + (-kx*scale*si + ky*scale*co);
      ys = yf + (kx*scale*co + ky*scale*si);

      for (int k = i; k < i + 9; ++k) {
        for (int l = j; l < j + 9; ++l) {
          // Get coords of sample point on the rotated axis
          sample_y = yf + (l*scale*co + k*scale*si);
          sample_x = xf + (-l*scale*si + k*scale*co);

          // Get the gaussian weighted x and y responses
          gauss_s1 = gaussianV2(xs - sample_x, ys - sample_y, 2.5f*scale);

          y1 = fRoundV2(sample_y - 0.5f);
          x1 = fRoundV2(sample_x - 0.5f);

          y2 = fRoundV2(sample_y + 0.5f);
          x2 = fRoundV2(sample_x + 0.5f);

          fx = sample_x - x1;
          fy = sample_y - y1;

          res1 = *(evolution_[level].Lx.ptr<float>(y1)+x1);
          res2 = *(evolution_[level].Lx.ptr<float>(y1)+x2);
          res3 = *(evolution_[level].Lx.ptr<float>(y2)+x1);
          res4 = *(evolution_[level].Lx.ptr<float>(y2)+x2);
          rx = (1.0f - fx)*(1.0f - fy)*res1 + fx*(1.0f - fy)*res2 + (1.0f - fx)*fy*res3 + fx*fy*res4;

          res1 = *(evolution_[level].Ly.ptr<float>(y1)+x1);
          res2 = *(evolution_[level].Ly.ptr<float>(y1)+x2);
          res3 = *(evolution_[level].Ly.ptr<float>(y2)+x1);
          res4 = *(evolution_[level].Ly.ptr<float>(y2)+x2);
          ry = (1.0f - fx)*(1.0f - fy)*res1 + fx*(1.0f - fy)*res2 + (1.0f - fx)*fy*res3 + fx*fy*res4;

          // Get the x and y derivatives on the rotated axis
          rry = gauss_s1*(rx*co + ry*si);
          rrx = gauss_s1*(-rx*si + ry*co);

          // Sum the derivatives to the cumulative descriptor
          dx += rrx;
          dy += rry;
          mdx += fabs(rrx);
          mdy += fabs(rry);
        }
      }

      // Add the values to the descriptor vector
      gauss_s2 = gaussianV2(cx - 2.0f, cy - 2.0f, 1.5f);
      desc[dcount++] = dx*gauss_s2;
      desc[dcount++] = dy*gauss_s2;
      desc[dcount++] = mdx*gauss_s2;
      desc[dcount++] = mdy*gauss_s2;

      len += (dx*dx + dy*dy + mdx*mdx + mdy*mdy)*gauss_s2*gauss_s2;

      j += 9;
    }

    i += 9;
  }

  // convert to unit vector
  len = sqrt(len);

  for (i = 0; i < dsize; i++) {
    desc[i] /= len;
  }
}

/* ************************************************************************* */
/**
 * @brief This method computes the rupright descriptor (not rotation invariant) of
 * the provided keypoint
 * @param kpt Input keypoint
 * @param desc Descriptor vector
 */
void Upright_MLDB_Full_Descriptor_InvokerV2::Get_Upright_MLDB_Full_Descriptor(const KeyPoint& kpt, unsigned char *desc) const {

  float di = 0.0, dx = 0.0, dy = 0.0;
  float ri = 0.0, rx = 0.0, ry = 0.0, xf = 0.0, yf = 0.0;
  float sample_x = 0.0, sample_y = 0.0, ratio = 0.0;
  int x1 = 0, y1 = 0, sample_step = 0, pattern_size = 0;
  int level = 0, nsamples = 0, scale = 0;
  int dcount1 = 0, dcount2 = 0;

  CV_DbgAssert(options_.descriptor_channels <= 3);

  // Matrices for the M-LDB descriptor: the dimensions are [grid size] by [channel size]
  float values_1[4][3];
  float values_2[9][3];
  float values_3[16][3];

  // Get the information from the keypoint
  level = kpt.class_id;
  ratio = evolution_[level].octave_ratio;
  scale = fRoundV2(0.5f*kpt.size / ratio);
  yf = kpt.pt.y / ratio;
  xf = kpt.pt.x / ratio;

  // First 2x2 grid
  pattern_size = options_.descriptor_pattern_size;
  sample_step = pattern_size;

  for (int i = -pattern_size; i < pattern_size; i += sample_step) {
    for (int j = -pattern_size; j < pattern_size; j += sample_step) {
      di = dx = dy = 0.0;
      nsamples = 0;

      for (int k = i; k < i + sample_step; k++) {
        for (int l = j; l < j + sample_step; l++) {

          // Get the coordinates of the sample point
          sample_y = yf + l*scale;
          sample_x = xf + k*scale;

          y1 = fRoundV2(sample_y);
          x1 = fRoundV2(sample_x);

          ri = *(evolution_[level].Lt.ptr<float>(y1)+x1);
          rx = *(evolution_[level].Lx.ptr<float>(y1)+x1);
          ry = *(evolution_[level].Ly.ptr<float>(y1)+x1);

          di += ri;
          dx += rx;
          dy += ry;
          nsamples++;
        }
      }

      di /= nsamples;
      dx /= nsamples;
      dy /= nsamples;

      values_1[dcount2][0] = di;
      values_1[dcount2][1] = dx;
      values_1[dcount2][2] = dy;
      dcount2++;
    }
  }

  // Do binary comparison first level
  for (int i = 0; i < 4; i++) {
    for (int j = i + 1; j < 4; j++) {
      if (values_1[i][0] > values_1[j][0]) {
        desc[dcount1 / 8] |= (1 << (dcount1 % 8));
      }
      else {
        desc[dcount1 / 8] &= ~(1 << (dcount1 % 8));
      }
      dcount1++;

      if (values_1[i][1] > values_1[j][1]) {
        desc[dcount1 / 8] |= (1 << (dcount1 % 8));
      }
      else {
        desc[dcount1 / 8] &= ~(1 << (dcount1 % 8));
      }
      dcount1++;

      if (values_1[i][2] > values_1[j][2]) {
        desc[dcount1 / 8] |= (1 << (dcount1 % 8));
      }
      else {
        desc[dcount1 / 8] &= ~(1 << (dcount1 % 8));
      }
      dcount1++;
    }
  }

  // Second 3x3 grid
  sample_step = static_cast<int>(ceil(pattern_size*2. / 3.));
  dcount2 = 0;

  for (int i = -pattern_size; i < pattern_size; i += sample_step) {
    for (int j = -pattern_size; j < pattern_size; j += sample_step) {
      di = dx = dy = 0.0;
      nsamples = 0;

      for (int k = i; k < i + sample_step; k++) {
        for (int l = j; l < j + sample_step; l++) {

          // Get the coordinates of the sample point
          sample_y = yf + l*scale;
          sample_x = xf + k*scale;

          y1 = fRoundV2(sample_y);
          x1 = fRoundV2(sample_x);

          ri = *(evolution_[level].Lt.ptr<float>(y1)+x1);
          rx = *(evolution_[level].Lx.ptr<float>(y1)+x1);
          ry = *(evolution_[level].Ly.ptr<float>(y1)+x1);

          di += ri;
          dx += rx;
          dy += ry;
          nsamples++;
        }
      }

      di /= nsamples;
      dx /= nsamples;
      dy /= nsamples;

      values_2[dcount2][0] = di;
      values_2[dcount2][1] = dx;
      values_2[dcount2][2] = dy;
      dcount2++;
    }
  }

  //Do binary comparison second level
  dcount2 = 0;
  for (int i = 0; i < 9; i++) {
    for (int j = i + 1; j < 9; j++) {
      if (values_2[i][0] > values_2[j][0]) {
        desc[dcount1 / 8] |= (1 << (dcount1 % 8));
      }
      else {
        desc[dcount1 / 8] &= ~(1 << (dcount1 % 8));
      }
      dcount1++;

      if (values_2[i][1] > values_2[j][1]) {
        desc[dcount1 / 8] |= (1 << (dcount1 % 8));
      }
      else {
        desc[dcount1 / 8] &= ~(1 << (dcount1 % 8));
      }
      dcount1++;

      if (values_2[i][2] > values_2[j][2]) {
        desc[dcount1 / 8] |= (1 << (dcount1 % 8));
      }
      else {
        desc[dcount1 / 8] &= ~(1 << (dcount1 % 8));
      }
      dcount1++;
    }
  }

  // Third 4x4 grid
  sample_step = pattern_size / 2;
  dcount2 = 0;

  for (int i = -pattern_size; i < pattern_size; i += sample_step) {
    for (int j = -pattern_size; j < pattern_size; j += sample_step) {
      di = dx = dy = 0.0;
      nsamples = 0;

      for (int k = i; k < i + sample_step; k++) {
        for (int l = j; l < j + sample_step; l++) {

          // Get the coordinates of the sample point
          sample_y = yf + l*scale;
          sample_x = xf + k*scale;

          y1 = fRoundV2(sample_y);
          x1 = fRoundV2(sample_x);

          ri = *(evolution_[level].Lt.ptr<float>(y1)+x1);
          rx = *(evolution_[level].Lx.ptr<float>(y1)+x1);
          ry = *(evolution_[level].Ly.ptr<float>(y1)+x1);

          di += ri;
          dx += rx;
          dy += ry;
          nsamples++;
        }
      }

      di /= nsamples;
      dx /= nsamples;
      dy /= nsamples;

      values_3[dcount2][0] = di;
      values_3[dcount2][1] = dx;
      values_3[dcount2][2] = dy;
      dcount2++;
    }
  }

  //Do binary comparison third level
  dcount2 = 0;
  for (int i = 0; i < 16; i++) {
    for (int j = i + 1; j < 16; j++) {
      if (values_3[i][0] > values_3[j][0]) {
        desc[dcount1 / 8] |= (1 << (dcount1 % 8));
      }
      else {
        desc[dcount1 / 8] &= ~(1 << (dcount1 % 8));
      }
      dcount1++;

      if (values_3[i][1] > values_3[j][1]) {
        desc[dcount1 / 8] |= (1 << (dcount1 % 8));
      }
      else {
        desc[dcount1 / 8] &= ~(1 << (dcount1 % 8));
      }
      dcount1++;

      if (values_3[i][2] > values_3[j][2]) {
        desc[dcount1 / 8] |= (1 << (dcount1 % 8));
      }
      else {
        desc[dcount1 / 8] &= ~(1 << (dcount1 % 8));
      }
      dcount1++;
    }
  }
}

inline
void MLDB_Full_Descriptor_InvokerV2::MLDB_Fill_Values(float* values, int sample_step, int level,
                                                    float xf, float yf, float co, float si, float scale) const
{
    int pattern_size = options_.descriptor_pattern_size;
    int chan = options_.descriptor_channels;
    int valpos = 0;

    for (int i = -pattern_size; i < pattern_size; i += sample_step) {
        for (int j = -pattern_size; j < pattern_size; j += sample_step) {
            float di, dx, dy;
            di = dx = dy = 0.0;
            int nsamples = 0;

            for (int k = i; k < i + sample_step; k++) {
              for (int l = j; l < j + sample_step; l++) {
                float sample_y = yf + (l*co * scale + k*si*scale);
                float sample_x = xf + (-l*si * scale + k*co*scale);

                int y1 = fRoundV2(sample_y);
                int x1 = fRoundV2(sample_x);

                float ri = *(evolution_[level].Lt.ptr<float>(y1)+x1);
                di += ri;

                if(chan > 1) {
                    float rx = *(evolution_[level].Lx.ptr<float>(y1)+x1);
                    float ry = *(evolution_[level].Ly.ptr<float>(y1)+x1);
                    if (chan == 2) {
                      dx += sqrtf(rx*rx + ry*ry);
                    }
                    else {
                      float rry = rx*co + ry*si;
                      float rrx = -rx*si + ry*co;
                      dx += rrx;
                      dy += rry;
                    }
                }
                nsamples++;
              }
            }
            di /= nsamples;
            dx /= nsamples;
            dy /= nsamples;

            values[valpos] = di;
            if (chan > 1) {
                values[valpos + 1] = dx;
            }
            if (chan > 2) {
              values[valpos + 2] = dy;
            }
            valpos += chan;
          }
        }
}

void MLDB_Full_Descriptor_InvokerV2::MLDB_Binary_Comparisons(float* values, unsigned char* desc,
                                                           int count, int& dpos) const {
    int chan = options_.descriptor_channels;
    int* ivalues = (int*) values;
    for(int i = 0; i < count * chan; i++) {
        ivalues[i] = CV_TOGGLE_FLT(ivalues[i]);
    }

    for(int pos = 0; pos < chan; pos++) {
        for (int i = 0; i < count; i++) {
            int ival = ivalues[chan * i + pos];
            for (int j = i + 1; j < count; j++) {
                if (ival > ivalues[chan * j + pos]) {
                    desc[dpos >> 3] |= (1 << (dpos & 7));
                }
                else {
                    desc[dpos >> 3] &= ~(1 << (dpos & 7));
                }
                dpos++;
            }
        }
    }
}

/* ************************************************************************* */
/**
 * @brief This method computes the descriptor of the provided keypoint given the
 * main orientation of the keypoint
 * @param kpt Input keypoint
 * @param desc Descriptor vector
 */
void MLDB_Full_Descriptor_InvokerV2::Get_MLDB_Full_Descriptor(const KeyPoint& kpt, unsigned char *desc) const {

  const int max_channels = 3;
  CV_Assert(options_.descriptor_channels <= max_channels);
  float values[16*max_channels];
  const double size_mult[3] = {1, 2.0/3.0, 1.0/2.0};

  float ratio = evolution_[kpt.class_id].octave_ratio;
  float scale = (float)fRoundV2(0.5f*kpt.size / ratio);
  float xf = kpt.pt.x / ratio;
  float yf = kpt.pt.y / ratio;
  float co = cos(kpt.angle);
  float si = sin(kpt.angle);
  int pattern_size = options_.descriptor_pattern_size;

  int dpos = 0;
  for(int lvl = 0; lvl < 3; lvl++) {

      int val_count = (lvl + 2) * (lvl + 2);
      int sample_step = static_cast<int>(ceil(pattern_size * size_mult[lvl]));
      MLDB_Fill_Values(values, sample_step, kpt.class_id, xf, yf, co, si, scale);
      MLDB_Binary_Comparisons(values, desc, val_count, dpos);
  }
}

/* ************************************************************************* */
/**
 * @brief This method computes the M-LDB descriptor of the provided keypoint given the
 * main orientation of the keypoint. The descriptor is computed based on a subset of
 * the bits of the whole descriptor
 * @param kpt Input keypoint
 * @param desc Descriptor vector
 */
void MLDB_Descriptor_Subset_InvokerV2::Get_MLDB_Descriptor_Subset(const KeyPoint& kpt, unsigned char *desc) const {

  const TEvolutionV2 & e = evolution_[kpt.class_id];

  // Get the information from the keypoint
  int scale = fRoundV2(0.5f*kpt.size / e.octave_ratio);
  float yf = kpt.pt.y / e.octave_ratio;
  float xf = kpt.pt.x / e.octave_ratio;
  float co = cos(kpt.angle);
  float si = sin(kpt.angle);
  int level = kpt.class_id;

  // Matrices for the M-LDB descriptor: the size is [grid size] * [channel size]
  CV_DbgAssert(descriptorSamples_.rows <= (4 + 9 + 16));
  CV_DbgAssert(options_.descriptor_channels <= 3);
  float values[(4 + 9 + 16)*3];

  // Sample everything, but only do the comparisons

  for (int i = 0; i < descriptorSamples_.rows; i++) {
    const int *coords = descriptorSamples_.ptr<int>(i);

    int sample_step = coords[0];

    float di = 0.0f;
    float dx = 0.0f;
    float dy = 0.0f;

    for (int x = coords[1]; x < coords[1] + coords[0]; x++) {
      for (int y = coords[2]; y < coords[2] + coords[0]; y++) {

        // Get the coordinates of the sample point
        int x1 = fRoundV2(xf + (x*scale*co - y*scale*si));
        int y1 = fRoundV2(yf + (x*scale*si + y*scale*co));

        di += *(e.Lt.ptr<float>(y1)+x1);

        if (options_.descriptor_channels > 1) {
          float rx = *(e.Lx.ptr<float>(y1)+x1);
          float ry = *(e.Ly.ptr<float>(y1)+x1);

          if (options_.descriptor_channels == 2) {
            dx += sqrtf(rx*rx + ry*ry);
          }
          else if (options_.descriptor_channels == 3) {
            // Get the x and y derivatives on the rotated axis
            dx += rx*co + ry*si;
            dy += -rx*si + ry*co;
          }
        }
      }
    }

    values[i * options_.descriptor_channels] = di;

    if (options_.descriptor_channels == 2) {
      values[i * options_.descriptor_channels + 1] = dx;
    }
    else if (options_.descriptor_channels == 3) {
      values[i * options_.descriptor_channels + 1] = dx;
      values[i * options_.descriptor_channels + 2] = dy;
    }
  }

  // Do the comparisons
  const int *comps = descriptorBits_.ptr<int>(0);

  for (int i = 0; i<descriptorBits_.rows; i++) {
    if (values[comps[2 * i]] > values[comps[2 * i + 1]]) {
      desc[i / 8] |= (1 << (i % 8));
    }
    else {
      desc[i / 8] &= ~(1 << (i % 8));
    }
  }
}

/* ************************************************************************* */
/**
 * @brief This method computes the upright (not rotation invariant) M-LDB descriptor
 * of the provided keypoint given the main orientation of the keypoint.
 * The descriptor is computed based on a subset of the bits of the whole descriptor
 * @param kpt Input keypoint
 * @param desc Descriptor vector
 */
void Upright_MLDB_Descriptor_Subset_InvokerV2::Get_Upright_MLDB_Descriptor_Subset(const KeyPoint& kpt, unsigned char *desc) const {

  // Get the information from the keypoint
  float ratio = evolution_[kpt.class_id].octave_ratio;
  int scale = fRoundV2(0.5f*kpt.size / ratio);
  int level = kpt.class_id;
  float yf = kpt.pt.y / ratio;
  float xf = kpt.pt.x / ratio;

  // Matrices for the M-LDB descriptor: the size is [grid size] * [channel size]
  CV_DbgAssert(descriptorSamples_.rows <= (4 + 9 + 16));
  CV_DbgAssert(options_.descriptor_channels <= 3);
  float values[(4 + 9 + 16)*3];

  for (int i = 0; i < descriptorSamples_.rows; i++) {
    const int *coords = descriptorSamples_.ptr<int>(i);

    float di = 0.0f;
    float dx = 0.0f;
    float dy = 0.0f;

    for (int x = coords[1]; x < coords[1] + coords[0]; x++) {
      for (int y = coords[2]; y < coords[2] + coords[0]; y++) {

        // Get the coordinates of the sample point
        int x1 = fRoundV2(xf + x*scale);
        int y1 = fRoundV2(yf + y*scale);

        di += *(evolution_[level].Lt.ptr<float>(y1)+x1);

        if (options_.descriptor_channels > 1) {
          float rx = *(evolution_[level].Lx.ptr<float>(y1)+x1);
          float ry = *(evolution_[level].Ly.ptr<float>(y1)+x1);

          if (options_.descriptor_channels == 2) {
            dx += sqrtf(rx*rx + ry*ry);
          }
          else if (options_.descriptor_channels == 3) {
            dx += rx;
            dy += ry;
          }
        }
      }
    }

    values[i * options_.descriptor_channels] = di;

    if (options_.descriptor_channels == 2) {
      values[i * options_.descriptor_channels + 1] = dx;
    }
    else if (options_.descriptor_channels == 3) {
      values[i * options_.descriptor_channels + 1] = dx;
      values[i * options_.descriptor_channels + 2] = dy;
    }
  }

  // Do the comparisons
  const int *comps = descriptorBits_.ptr<int>(0);

  for (int i = 0; i<descriptorBits_.rows; i++) {
    if (values[comps[2 * i]] > values[comps[2 * i + 1]]) {
      desc[i / 8] |= (1 << (i % 8));
    }
    else {
      desc[i / 8] &= ~(1 << (i % 8));
    }
  }
}

/* ************************************************************************* */
/**
 * @brief This function computes a (quasi-random) list of bits to be taken
 * from the full descriptor. To speed the extraction, the function creates
 * a list of the samples that are involved in generating at least a bit (sampleList)
 * and a list of the comparisons between those samples (comparisons)
 * @param sampleList
 * @param comparisons The matrix with the binary comparisons
 * @param nbits The number of bits of the descriptor
 * @param pattern_size The pattern size for the binary descriptor
 * @param nchannels Number of channels to consider in the descriptor (1-3)
 * @note The function keeps the 18 bits (3-channels by 6 comparisons) of the
 * coarser grid, since it provides the most robust estimations
 */
static
void generateDescriptorSubsampleV2(Mat& sampleList, Mat& comparisons, int nbits,
                                 int pattern_size, int nchannels) {

#if 0
  // Replaced by an immediate to use stack; need C++11 constexpr to use the logic
  int fullM_rows = 0;
  for (int i = 0; i < 3; i++) {
    int gz = (i + 2)*(i + 2);
    fullM_rows += gz*(gz - 1) / 2;
  }
#else
  const int fullM_rows = 162;
#endif

  int ssz = fullM_rows * nchannels; // ssz is 486 when nchannels is 3

  CV_Assert(nbits <= ssz); // Descriptor size can't be bigger than full descriptor

  const int steps[3] = {
    pattern_size,
    (int)ceil(2.f * pattern_size / 3.f),
    pattern_size / 2
  };

  // Since the full descriptor is usually under 10k elements, we pick
  // the selection from the full matrix.  We take as many samples per
  // pick as the number of channels. For every pick, we
  // take the two samples involved and put them in the sampling list

  int fullM_stack[fullM_rows * 5]; // About 6.3KB workspace with 64-bit int on stack
  Mat_<int> fullM(fullM_rows, 5, fullM_stack);

  for (int i = 0, c = 0; i < 3; i++) {
    int gdiv = i + 2; //grid divisions, per row
    int gsz = gdiv*gdiv;
    int psz = (int)ceil(2.f*pattern_size / (float)gdiv);

    for (int j = 0; j < gsz; j++) {
      for (int k = j + 1; k < gsz; k++, c++) {
        fullM(c, 0) = steps[i];
        fullM(c, 1) = psz*(j % gdiv) - pattern_size;
        fullM(c, 2) = psz*(j / gdiv) - pattern_size;
        fullM(c, 3) = psz*(k % gdiv) - pattern_size;
        fullM(c, 4) = psz*(k / gdiv) - pattern_size;
      }
    }
  }

  int comps_stack[486 * 2]; // About 7.6KB workspace with 64-bit int on stack
  Mat_<int> comps(486, 2, comps_stack);
  comps = 1000;

  int samples_stack[(4 + 9 + 16) * 3]; // 696 bytes workspace with 64-bit int on stack
  Mat_<int> samples((4 + 9 + 16), 3, samples_stack);

  // Select some samples. A sample includes all channels
  int count = 0;
  int npicks = (int)ceil(nbits / (float)nchannels);
  samples = -1;

  srand(1024);
  for (int i = 0; i < npicks; i++) {
    int k = rand() % (fullM_rows - i);
    if (i < 6) {
      // Force use of the coarser grid values and comparisons
      k = i;
    }

    bool n = true;

    for (int j = 0; j < count; j++) {
      if (samples(j, 0) == fullM(k, 0) && samples(j, 1) == fullM(k, 1) && samples(j, 2) == fullM(k, 2)) {
        n = false;
        comps(i*nchannels, 0) = nchannels*j;
        comps(i*nchannels + 1, 0) = nchannels*j + 1;
        comps(i*nchannels + 2, 0) = nchannels*j + 2;
        break;
      }
    }

    if (n) {
      samples(count, 0) = fullM(k, 0);
      samples(count, 1) = fullM(k, 1);
      samples(count, 2) = fullM(k, 2);
      comps(i*nchannels, 0) = nchannels*count;
      comps(i*nchannels + 1, 0) = nchannels*count + 1;
      comps(i*nchannels + 2, 0) = nchannels*count + 2;
      count++;
    }

    n = true;
    for (int j = 0; j < count; j++) {
      if (samples(j, 0) == fullM(k, 0) && samples(j, 1) == fullM(k, 3) && samples(j, 2) == fullM(k, 4)) {
        n = false;
        comps(i*nchannels, 1) = nchannels*j;
        comps(i*nchannels + 1, 1) = nchannels*j + 1;
        comps(i*nchannels + 2, 1) = nchannels*j + 2;
        break;
      }
    }

    if (n) {
      samples(count, 0) = fullM(k, 0);
      samples(count, 1) = fullM(k, 3);
      samples(count, 2) = fullM(k, 4);
      comps(i*nchannels, 1) = nchannels*count;
      comps(i*nchannels + 1, 1) = nchannels*count + 1;
      comps(i*nchannels + 2, 1) = nchannels*count + 2;
      count++;
    }

    fullM.row(fullM.rows - i - 1).copyTo(fullM.row(k));
  }

  sampleList = samples.rowRange(0, count).clone();
  comparisons = comps.rowRange(0, nbits).clone();
}

}
