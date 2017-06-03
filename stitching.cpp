#ifndef _APP_H
#define _APP_H

#include <iostream>
#include <fstream>
#include <string>

#include "opencv2/opencv_modules.hpp"
#include "opencv2/highgui/highgui.hpp"
#include "opencv2/stitching/detail/autocalib.hpp"
#include "opencv2/stitching/detail/blenders.hpp"
#include "opencv2/stitching/detail/camera.hpp"
#include "opencv2/stitching/detail/exposure_compensate.hpp"
#include "opencv2/stitching/detail/matchers.hpp"
#include "opencv2/stitching/detail/motion_estimators.hpp"
#include "opencv2/stitching/detail/seam_finders.hpp"
#include "opencv2/stitching/detail/util.hpp"
#include "opencv2/stitching/detail/warpers.hpp"
#include "opencv2/stitching/warpers.hpp"
#include "conf.cpp"
#include "Logger.h"

#endif

/**
 * 全局变量
 */
Logger Log;

int num_images;
double work_scale = 1;
double seam_scale = 1;
double compose_scale = 1;
double seam_work_aspect = 1;

bool is_work_scale_set = false;
bool is_seam_scale_set = false;
bool is_compose_scale_set = false;

Mat full_img, img;
vector<Mat> images;
vector<Size> full_img_sizes;

vector<int> indices;
vector<CameraParams> cameras;
float warped_image_scale;
vector<Mat> masks;
vector<Size> sizes;
vector<Point> corners;
vector<Mat> masks_warped;
vector<Mat> images_warped;
vector<Mat> images_warped_f;
Ptr<RotationWarper> warper;
Ptr<WarperCreator> warper_creator;

char* tmpUsedTime;

using namespace std;
using namespace cv;
using namespace cv::detail;
using namespace conf;

/**
 * @brief 特征提取
 * @return
 */
vector<ImageFeatures> extractFeature()
{

    Log.info("Extract Feature Start..");
    int64 start = getTickCount();

    Ptr<FeaturesFinder> finder;
    if (features_type == "surf")
    {
#if defined(HAVE_OPENCV_NONFREE) && defined(HAVE_OPENCV_GPU)
        if (try_gpu && gpu::getCudaEnabledDeviceCount() > 0)
            finder = new SurfFeaturesFinderGpu();
        else
#endif
            finder = new SurfFeaturesFinder();
    }

    vector<ImageFeatures> features(num_images);

    vector<Mat> _images(num_images);
    images.assign(_images.begin(), _images.end());

    vector<Size> _full_img_sizes(num_images);
    full_img_sizes.assign(_full_img_sizes.begin(), _full_img_sizes.end());

    for (int i = 0; i < num_images; ++i)
    {
        full_img = imread(img_names[i]);
        full_img_sizes[i] = full_img.size();

        if (full_img.empty())
        {
            Log.error("Open image failed" + img_names[i]);
            exit;
        }
        if (work_megapix < 0)
        {
            img = full_img;
            work_scale = 1;
            is_work_scale_set = true;
        }
        else
        {
            if (!is_work_scale_set)
            {
                work_scale = min(1.0, sqrt(work_megapix * 1e6 / full_img.size().area()));
                is_work_scale_set = true;
            }
            resize(full_img, img, Size(), work_scale, work_scale);
        }
        if (!is_seam_scale_set)
        {
            seam_scale = min(1.0, sqrt(seam_megapix * 1e6 / full_img.size().area()));
            seam_work_aspect = seam_scale / work_scale;
            is_seam_scale_set = true;
        }

        (*finder)(img, features[i]);
        features[i].img_idx = i;

        resize(full_img, img, Size(), seam_scale, seam_scale);
        images[i] = img.clone();
    }

    finder->collectGarbage();
    full_img.release();
    img.release();

    Log.info("Extract Feature End..");
    sprintf(tmpUsedTime, "%f", (getTickCount() - start) / getTickFrequency());
    Log.info("Used Time:" + (string)tmpUsedTime + " sec");

    return features;
}

/**
 * @brief 特征匹配
 * @param features
 * @return
 */
vector<MatchesInfo> matchFeature (vector<ImageFeatures> features)
{
    Log.info("Feature Matching Start");

    int64 start = getTickCount();
    vector<MatchesInfo> pairwise_matches;
    BestOf2NearestMatcher matcher(try_gpu, match_conf);
    matcher(features, pairwise_matches);
    matcher.collectGarbage();

    Log.info("Feature Matching End");
    sprintf(tmpUsedTime, "%f", (getTickCount() - start) / getTickFrequency());
    Log.info("Used Time: " + (string)tmpUsedTime + " sec");

    return pairwise_matches;
}

/**
 * @brief 还原图像序列
 * @param pairwise_matches
 */
void recoverOrder(vector<ImageFeatures> features, vector<MatchesInfo> pairwise_matches)
{
    indices = leaveBiggestComponent(features, pairwise_matches, conf_thresh);
    vector<Mat> img_subset;
    vector<string> img_names_subset;
    vector<Size> full_img_sizes_subset;
    for (size_t i = 0; i < indices.size(); ++i)
    {
        img_names_subset.push_back(img_names[indices[i]]);
        img_subset.push_back(images[indices[i]]);
        full_img_sizes_subset.push_back(full_img_sizes[indices[i]]);
    }

    images = img_subset;
    img_names = img_names_subset;
    full_img_sizes = full_img_sizes_subset;
}

/**
 * @brief 参数估计
 */
void estimate(vector<ImageFeatures> features, vector<MatchesInfo> pairwise_matches)
{
    HomographyBasedEstimator estimator;
    vector<CameraParams> cameras;
    estimator(features, pairwise_matches, cameras);

    for (size_t i = 0; i < cameras.size(); ++i)
    {
        Mat R;
        cameras[i].R.convertTo(R, CV_32F);
        cameras[i].R = R;
    }

    Ptr<detail::BundleAdjusterBase> adjuster;
    if (ba_cost_func == "reproj") adjuster = new detail::BundleAdjusterReproj();
    else if (ba_cost_func == "ray") adjuster = new detail::BundleAdjusterRay();
    else
    {
        Log.error("Unknown bundle adjustment cost function: '" + ba_cost_func + "'.\n");
        exit;
    }

    adjuster->setConfThresh(conf_thresh);
    Mat_<uchar> refine_mask = Mat::zeros(3, 3, CV_8U);
    if (ba_refine_mask[0] == 'x') refine_mask(0,0) = 1;
    if (ba_refine_mask[1] == 'x') refine_mask(0,1) = 1;
    if (ba_refine_mask[2] == 'x') refine_mask(0,2) = 1;
    if (ba_refine_mask[3] == 'x') refine_mask(1,1) = 1;
    if (ba_refine_mask[4] == 'x') refine_mask(1,2) = 1;
    adjuster->setRefinementMask(refine_mask);
    (*adjuster)(features, pairwise_matches, cameras);

    // 焦距估计
    vector<double> focals;
    for (size_t i = 0; i < cameras.size(); ++i)
    {
        focals.push_back(cameras[i].focal);
    }

    sort(focals.begin(), focals.end());
    if (focals.size() % 2 == 1)
    {
        warped_image_scale = static_cast<float>(focals[focals.size() / 2]);
    } else {
        warped_image_scale = static_cast<float>(focals[focals.size() / 2 - 1] + focals[focals.size() / 2]) * 0.5f;
    }

    // 波形校正
    if (do_wave_correct)
    {
        vector<Mat> rmats;
        for (size_t i = 0; i < cameras.size(); ++i)
        {
            rmats.push_back(cameras[i].R);
        }

        waveCorrect(rmats, wave_correct);
        for (size_t i = 0; i < cameras.size(); ++i)
        {
            cameras[i].R = rmats[i];
        }
    }
}

/**
 * @brief wrap
 */
void wrap()
{
    Log.info("Warping images (auxiliary)");

    int64 start = getTickCount();

    vector<Point> _corners(num_images);
    corners.assign(_corners.begin(), _corners.end());

    vector<Mat> _masks_warped(num_images);
    masks_warped.assign(_masks_warped.begin(), _masks_warped.end());

    vector<Mat> _images_warped(num_images);
    images_warped.assign(_images_warped.begin(), _images_warped.end());

    vector<Size> _sizes(num_images);
    sizes.assign(_sizes.begin(), _sizes.end());

    vector<Mat> _masks(num_images);
    masks.assign(_masks.begin(), _masks.end());

    // 准备拼接 Mask
    for (int i = 0; i < num_images; ++i)
    {
        masks[i].create(images[i].size(), CV_8U);
        masks[i].setTo(Scalar::all(255));
    }

    // 创建拼接面
#if defined(HAVE_OPENCV_GPU)
    if (try_gpu && gpu::getCudaEnabledDeviceCount() > 0)
    {
        if (warp_type == "plane") warper_creator = new cv::PlaneWarperGpu();
        else if (warp_type == "cylindrical") warper_creator = new cv::CylindricalWarperGpu();
        else if (warp_type == "spherical") warper_creator = new cv::SphericalWarperGpu();
    }
    else
#endif
    {
        if (warp_type == "plane") warper_creator = new cv::PlaneWarper();
        else if (warp_type == "cylindrical") warper_creator = new cv::CylindricalWarper();
        else if (warp_type == "spherical") warper_creator = new cv::SphericalWarper();
        else if (warp_type == "fisheye") warper_creator = new cv::FisheyeWarper();
        else if (warp_type == "stereographic") warper_creator = new cv::StereographicWarper();
        else if (warp_type == "compressedPlaneA2B1") warper_creator = new cv::CompressedRectilinearWarper(2, 1);
        else if (warp_type == "compressedPlaneA1.5B1") warper_creator = new cv::CompressedRectilinearWarper(1.5, 1);
        else if (warp_type == "compressedPlanePortraitA2B1") warper_creator = new cv::CompressedRectilinearPortraitWarper(2, 1);
        else if (warp_type == "compressedPlanePortraitA1.5B1") warper_creator = new cv::CompressedRectilinearPortraitWarper(1.5, 1);
        else if (warp_type == "paniniA2B1") warper_creator = new cv::PaniniWarper(2, 1);
        else if (warp_type == "paniniA1.5B1") warper_creator = new cv::PaniniWarper(1.5, 1);
        else if (warp_type == "paniniPortraitA2B1") warper_creator = new cv::PaniniPortraitWarper(2, 1);
        else if (warp_type == "paniniPortraitA1.5B1") warper_creator = new cv::PaniniPortraitWarper(1.5, 1);
        else if (warp_type == "mercator") warper_creator = new cv::MercatorWarper();
        else if (warp_type == "transverseMercator") warper_creator = new cv::TransverseMercatorWarper();
    }

    if (warper_creator.empty())
    {
        Log.error("Can't create the following warper '" + warp_type);
        exit;
    }

    warper = warper_creator->create(static_cast<float>(warped_image_scale * seam_work_aspect));

    for (int i = 0; i < num_images; ++i)
    {
        Mat_<float> K;
        cameras[i].K().convertTo(K, CV_32F);
        float swa = (float)seam_work_aspect;
        K(0,0) *= swa; K(0,2) *= swa;
        K(1,1) *= swa; K(1,2) *= swa;

        corners[i] = warper->warp(images[i], K, cameras[i].R, INTER_LINEAR, BORDER_REFLECT, images_warped[i]);
        sizes[i] = images_warped[i].size();

        warper->warp(masks[i], K, cameras[i].R, INTER_NEAREST, BORDER_CONSTANT, masks_warped[i]);
    }

    vector<Mat> _images_warped_f(num_images);
    images_warped_f.assign(_images_warped_f.begin(), _images_warped_f.end());

    for (int i = 0; i < num_images; ++i)
    {
        images_warped[i].convertTo(images_warped_f[i], CV_32F);
    }
    sprintf(tmpUsedTime, "%f", (getTickCount() - start) / getTickFrequency());
    Log.info("Warping images, time: " + (string)tmpUsedTime + " sec");
}

int start(int argc, char* argv[])
{
    // 开始时间
    int64 app_start_time = getTickCount();

    int argsAvilable = parseCmdArgs(argc, argv);
    // 检查参数解析是否正确
    if (argsAvilable) {
       Log.error("Parse Command Arguments Filed.");
       return argsAvilable;
    }

    // 检查图片数量是否 > 1
    num_images = static_cast<int>(img_names.size());
    if (num_images < 2)
    {
        Log.error("Need more images");
        return -1;
    }

    // 特征提取
    vector<ImageFeatures> features = extractFeature();

    // 特征匹配
    vector<MatchesInfo> pairwise_matches = matchFeature(features);

    // 是否保存匹配结果
    if (save_graph)
    {
        Log.info("Saving Matches Start");
        ofstream f(save_graph_to.c_str());
        f << matchesGraphAsString(img_names, pairwise_matches, conf_thresh);
        Log.info("Saving Matches End");
    }

    recoverOrder(features, pairwise_matches);

    // 序列中图像数量是否大于2
    num_images = static_cast<int>(img_names.size());
    if (num_images < 2)
    {
        Log.error("Need more images");
        return -1;
    }

    // 求单应性矩阵：匹配模型RANSAC提纯 / 参数估计 / 建立变换模型
    estimate(features, pairwise_matches);

    // 图像拼接
    wrap();

    // 缝隙估计
    Ptr<ExposureCompensator> compensator = ExposureCompensator::createDefault(expos_comp_type);
    compensator->feed(corners, images_warped, masks_warped);

    Ptr<SeamFinder> seam_finder;
    if (seam_find_type == "no")
        seam_finder = new detail::NoSeamFinder();
    else if (seam_find_type == "voronoi")
        seam_finder = new detail::VoronoiSeamFinder();
    else if (seam_find_type == "gc_color")
    {
#if defined(HAVE_OPENCV_GPU)
        if (try_gpu && gpu::getCudaEnabledDeviceCount() > 0)
            seam_finder = new detail::GraphCutSeamFinderGpu(GraphCutSeamFinderBase::COST_COLOR);
        else
#endif
            seam_finder = new detail::GraphCutSeamFinder(GraphCutSeamFinderBase::COST_COLOR);
    }
    else if (seam_find_type == "gc_colorgrad")
    {
#if defined(HAVE_OPENCV_GPU)
        if (try_gpu && gpu::getCudaEnabledDeviceCount() > 0)
            seam_finder = new detail::GraphCutSeamFinderGpu(GraphCutSeamFinderBase::COST_COLOR_GRAD);
        else
#endif
            seam_finder = new detail::GraphCutSeamFinder(GraphCutSeamFinderBase::COST_COLOR_GRAD);
    }
    else if (seam_find_type == "dp_color")
        seam_finder = new detail::DpSeamFinder(DpSeamFinder::COLOR);
    else if (seam_find_type == "dp_colorgrad")
        seam_finder = new detail::DpSeamFinder(DpSeamFinder::COLOR_GRAD);
    if (seam_finder.empty())
    {
        Log.error("Can't create the following seam finder '" + seam_find_type);
        return 1;
    }

    seam_finder->find(images_warped_f, corners, masks_warped);

    // Release unused memory
    images.clear();
    images_warped.clear();
    images_warped_f.clear();
    masks.clear();

    // 融合
    Log.info("Compositing");

    int64 start = getTickCount();
    Mat img_warped, img_warped_s;
    Mat dilated_mask, seam_mask, mask, mask_warped;
    Ptr<Blender> blender;
    //double compose_seam_aspect = 1;
    double compose_work_aspect = 1;

    for (int img_idx = 0; img_idx < num_images; ++img_idx)
    {
        Log.info("Compositing image #" + indices[img_idx]+1);
        full_img = imread(img_names[img_idx]);
        if (!is_compose_scale_set)
        {
            if (compose_megapix > 0)
                compose_scale = min(1.0, sqrt(compose_megapix * 1e6 / full_img.size().area()));
            is_compose_scale_set = true;

            // Compute relative scales
            compose_work_aspect = compose_scale / work_scale;

            warped_image_scale *= static_cast<float>(compose_work_aspect);
            warper = warper_creator->create(warped_image_scale);

            // Update corners and sizes
            for (int i = 0; i < num_images; ++i)
            {
                // Update intrinsics
                cameras[i].focal *= compose_work_aspect;
                cameras[i].ppx *= compose_work_aspect;
                cameras[i].ppy *= compose_work_aspect;

                // Update corner and size
                Size sz = full_img_sizes[i];
                if (std::abs(compose_scale - 1) > 1e-1)
                {
                    sz.width = cvRound(full_img_sizes[i].width * compose_scale);
                    sz.height = cvRound(full_img_sizes[i].height * compose_scale);
                }

                Mat K;
                cameras[i].K().convertTo(K, CV_32F);
                Rect roi = warper->warpRoi(sz, K, cameras[i].R);
                corners[i] = roi.tl();
                sizes[i] = roi.size();
            }
        }
        if (abs(compose_scale - 1) > 1e-1)
        {
            resize(full_img, img, Size(), compose_scale, compose_scale);
        } else {
            img = full_img;
        }
        full_img.release();
        Size img_size = img.size();

        Mat K;
        cameras[img_idx].K().convertTo(K, CV_32F);

        // Warp the current image
        warper->warp(img, K, cameras[img_idx].R, INTER_LINEAR, BORDER_REFLECT, img_warped);

        // Warp the current image mask
        mask.create(img_size, CV_8U);
        mask.setTo(Scalar::all(255));
        warper->warp(mask, K, cameras[img_idx].R, INTER_NEAREST, BORDER_CONSTANT, mask_warped);

        // Compensate exposure
        compensator->apply(img_idx, corners[img_idx], img_warped, mask_warped);

        img_warped.convertTo(img_warped_s, CV_16S);
        img_warped.release();
        img.release();
        mask.release();

        dilate(masks_warped[img_idx], dilated_mask, Mat());
        resize(dilated_mask, seam_mask, mask_warped.size());
        mask_warped = seam_mask & mask_warped;

        if (blender.empty())
        {
            blender = Blender::createDefault(blend_type, try_gpu);
            Size dst_sz = resultRoi(corners, sizes).size();
            float blend_width = sqrt(static_cast<float>(dst_sz.area())) * blend_strength / 100.f;
            if (blend_width < 1.f)
                blender = Blender::createDefault(Blender::NO, try_gpu);
            else if (blend_type == Blender::MULTI_BAND)
            {
                MultiBandBlender* mb = dynamic_cast<MultiBandBlender*>(static_cast<Blender*>(blender));
                mb->setNumBands(static_cast<int>(ceil(log(blend_width)/log(2.)) - 1.));
            }
            else if (blend_type == Blender::FEATHER)
            {
                FeatherBlender* fb = dynamic_cast<FeatherBlender*>(static_cast<Blender*>(blender));
                fb->setSharpness(1.f/blend_width);
            }
            blender->prepare(corners, sizes);
        }

        // Blend the current image
        blender->feed(img_warped_s, mask_warped, corners[img_idx]);
    }

    Mat result, result_mask;
    blender->blend(result, result_mask);

    sprintf(tmpUsedTime, "%f", (getTickCount() - start) / getTickFrequency());
    Log.info("Compositing, used time: " + (string)tmpUsedTime + " sec");

    imwrite(result_name, result);

    sprintf(tmpUsedTime, "%f", (getTickCount() - start) / getTickFrequency());
    Log.info("Finished, used time: " + (string)tmpUsedTime  + " sec");
    return 0;
}
