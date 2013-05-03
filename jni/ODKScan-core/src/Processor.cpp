#include "configuration.h"
#include "Processor.h"
#include "FileUtils.h"
#include "PCA_classifier.h"
#include "Aligner.h"
#include "SegmentAligner.h"
#include "AlignmentUtils.h"
#include "Addons.h"

#include <opencv2/core/core.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#include <opencv2/calib3d/calib3d.hpp>

#include <json/json.h>

//QRCode stuff:
#include "ImageReaderSource.h"
#include <zxing/common/Counted.h>
#include <zxing/Binarizer.h>
#include <zxing/MultiFormatReader.h>
#include <zxing/Result.h>
#include <zxing/ReaderException.h>
//#include <zxing/common/GlobalHistogramBinarizer.h>
#include <zxing/common/HybridBinarizer.h>
#include <exception>
#include <zxing/Exception.h>
#include <zxing/common/IllegalArgumentException.h>
#include <zxing/BinaryBitmap.h>
#include <zxing/DecodeHints.h>
#include <zxing/qrcode/QRCodeReader.h>
#include <zxing/multi/qrcode/QRCodeMultiReader.h>
#include <zxing/multi/ByQuadrantReader.h>
//#include <zxing/multi/MultipleBarcodeReader.h>
//#include <zxing/multi/GenericMultipleBarcodeReader.h>

using namespace zxing;
using namespace zxing::multi;
using namespace zxing::qrcode;

#include <iostream>
#include <fstream>
#include <map>
#include <time.h>
// This sets the resolution of the form at which to perform segment alignment and classification.
// It is a percentage of the size specified in the template.
#define SCALEPARAM 1.0

// Creates a buffer around segments proportional to their size.
// I think .5 is the largest value that won't cause ambiguous cases.
#define SEGMENT_BUFFER .25

#ifdef OUTPUT_BUBBLE_IMAGES
	#include "NameGenerator.h"
	NameGenerator namer;
#endif

//#define TIME_IT

#define REFINE_ALL_BUBBLE_LOCATIONS true

using namespace std;
using namespace cv;

//Iterates over a field's segments and items to determine it's value.
Json::Value computeFieldValue(const Json::Value& field){
	Json::Value output;
	const Json::Value segments = field["segments"];
	for ( size_t i = 0; i < segments.size(); i++ ) {
		const Json::Value segment = segments[i];
		const Json::Value items = segment["items"];
		if( items.isNull() ){
			//If the segment already has a value,
			//probably from a qrcode, use that.
			return segment.get("value", Json::Value());
		}
		for ( size_t j = 0; j < items.size(); j++ ) {
			const Json::Value classification = items[j].get("classification", false);
			const Json::Value itemValue = items[j]["value"];
			switch ( classification.type() )
			{
				case Json::stringValue:
					//This case isn't used right now.
					//It's for classifiers that picks up letters.
					//The idea is to concatenate all the letters into a word.
					output = Json::Value(output.asString() +
						             classification.asString());
				break;
				case Json::booleanValue:
					if(!itemValue.isNull()){
						//This case is for selects.
						//The values of the filled (i.e. true) items
						//are stored in a space delimited string.
						if(classification.asBool()){
							if( output.asString().length() == 0 ){
								output = Json::Value(itemValue.asString());
							}
							else{
								output = Json::Value(output.asString() + " " +
									     itemValue.asString());
							}
						}
						else {
							//Set the output to be a string.
							//If this is not done, we get a null when no bubbles are selected.
							output = Json::Value(output.asString());
						}
						break;
					}
					//Fall through and count the boolean as a 1 or 0
					//for a tally.
				case Json::intValue:
				case Json::uintValue:
					output = Json::Value(output.asInt() + classification.asInt());
				break;
				case Json::realValue:
					output = Json::Value(output.asDouble() + classification.asDouble());
				break;
				default:
				break;
			}
		}
	}
	return output;
}
Mat markupForm(const Json::Value& bvRoot, const Mat& inputImage, bool drawCounts) {
	Mat markupImage;
	cvtColor(inputImage, markupImage, CV_GRAY2RGB);
	const Json::Value fields = bvRoot["fields"];
	for ( size_t i = 0; i < fields.size(); i++ ) {
		const Json::Value field = fields[i];
		
		float avgWidth = 0;
		float avgY = 0;
		int endOfField = 0;

		Scalar boxColor = getColor(int(i));
	
		const Json::Value segments = field["segments"];
		
		for ( size_t j = 0; j < segments.size(); j++ ) {
			const Json::Value segment = segments[j];

			//Draw segment rectangles:
			vector<Point> quad = orderCorners( jsonArrayToQuad(segment["quad"]) );
			const Point* p = &quad[0];
			int n = (int) quad.size();
			polylines(markupImage, &p, &n, 1, true, boxColor, 1, CV_AA);
			
			if( segment.get("notFound", false).asBool() ) {
				polylines(markupImage, &p, &n, 1, true, .25 * boxColor, 1, CV_AA);
			}
			else{
				polylines(markupImage, &p, &n, 1, true, boxColor, 1, CV_AA);
			}
			
			//Compute some stuff to figure out where to draw output on the form
			avgWidth += norm(quad[0] - quad[1]);
			if(endOfField < quad[1].x){
				endOfField = quad[1].x;
			}
			avgY += quad[3].y;// + quad[1].y + quad[2].y + quad[3].y) / 4;
			

			const Json::Value items = segment["items"];
			for ( size_t k = 0; k < items.size(); k++ ) {
				const Json::Value Item = items[k];
				Point ItemLocation(jsonToPoint(Item["absolute_location"]));
				Json::Value classification = Item["classification"];

				if(classification.isBool()){
					circle(markupImage, ItemLocation, 2, 	getColor(classification.asBool()), 1, CV_AA);
				}
				else if(classification.isInt()){
					circle(markupImage, ItemLocation, 2, 	getColor(classification.asInt()), 1, CV_AA);
				}
				else{
					cout << "Don't know what this is: " << classification << endl;
				}
				
			}
		}
		if(field.isMember("value")){
			Point textBoxTL;
			if(drawCounts && avgWidth > 0){
				avgWidth /= segments.size();
				avgY /= segments.size();
				textBoxTL = Point(endOfField + 5, (int)avgY - 5);
			}
			if(field.isMember("markup_location")){
				textBoxTL = Point(field["markup_location"]["x"].asInt(), field["markup_location"]["y"].asInt());
			}
			stringstream ss;
			ss << field.get("value", "");
			string markupString(ss.str());
			markupString = markupString.substr(0, markupString.length() - 1);
			putText(markupImage, markupString, textBoxTL,
			        FONT_HERSHEY_SIMPLEX, 1., Scalar::all(0), 3, CV_AA);
			putText(markupImage, markupString, textBoxTL,
			        FONT_HERSHEY_SIMPLEX, 1., boxColor, 2, CV_AA);
		}
	}
	return markupImage;
}
Json::Value minifyJsonOutput(const Json::Value& JsonOutput){
	Json::Value minifiedOutput;
	Json::Value minifiedOutputFields;
	const Json::Value fields = JsonOutput["fields"];
	for ( size_t i = 0; i < fields.size(); i++ ) {
		Json::Value minifiedOutputField;
		Json::Value minifiedOutputSegments;
		const Json::Value field = fields[i];
		const Json::Value segments = field["segments"];
		for ( size_t j = 0; j < segments.size(); j++ ) {
			Json::Value minifiedOutputSegment;
			const Json::Value segment = segments[j];
			minifiedOutputSegment["image_path"] = segment["image_path"];
			minifiedOutputSegments.append(minifiedOutputSegment);
		}
		minifiedOutputField["segments"] = minifiedOutputSegments;
		if ( field.isMember("value") ) {
			minifiedOutputField["value"] = field["value"];
		}
		minifiedOutputField["name"] = field["name"];
		minifiedOutputFields.append(minifiedOutputField);
	}
	minifiedOutput["fields"] = minifiedOutputFields;
	return minifiedOutput;
}
// Get current date/time, format is YYYY-MM-DD.HH:mm:ss
// src: http://stackoverflow.com/questions/997946/c-get-current-time-and-date
const std::string currentDateTime() {
    time_t     now = time(0);
    struct tm  tstruct;
    char       buf[80];
    tstruct = *localtime(&now);
    // Visit http://www.cplusplus.com/reference/clibrary/ctime/strftime/
    // for more information about date/time format
    strftime(buf, sizeof(buf), "%Y-%m-%d.%X", &tstruct);
    return buf;
}
class Processor::ProcessorImpl
{
public:
	string trainingDataPath;
private:
	Mat formImage;

	Aligner aligner;

	Json::Value root;
	//Json::Value JsonOutput;

	typedef std::map<const std::string, cv::Ptr< PCA_classifier> > ClassiferMap;
	ClassiferMap classifiers;

	string templPath;
	//string imageDir;

	#ifdef TIME_IT
		clock_t init;
	#endif


//NOTE: training_data_uri must be a directory with no leading or trailing slashes.
Ptr<PCA_classifier>& getClassifier(const Json::Value& classifier) {

	const string& training_data_uri = classifier["training_data_uri"].asString();

	Size acutal_classifier_size = SCALEPARAM * Size(classifier["classifier_width"].asInt(),
	                                                classifier["classifier_height"].asInt());
	ostringstream ss;
	ss << acutal_classifier_size.height << 'x' << acutal_classifier_size.width;
	string key(training_data_uri + ss.str());

	ClassiferMap::iterator it = classifiers.find(key);
/*
	#ifdef DEBUG_PROCESSOR
		cout << "erasing old classifier (if it exisits)" << endl;
		if( it != classifiers.end() ) cout << "erased: " << classifiers.erase(key) << endl;
		cout << "erased" << endl;
	#endif
*/
	if( it == classifiers.end() ) {
		//PCA_classifier classifier = classifiers[key];
		classifiers[key] = Ptr<PCA_classifier>(new PCA_classifier);
		
		string dataPath = trainingDataPath + training_data_uri;
		string cachedDataPath = dataPath + "/cached_classifier_data_" + ss.str() + ".yml";

		classifiers[key]->set_classifier_params(classifier);

		try{
			classifiers[key]->load(cachedDataPath);
			#ifdef DEBUG_PROCESSOR
				cout << "found cached classifier..." << endl;
			#endif
		}
		catch(...){
			#ifdef DEBUG_PROCESSOR
				cout << "training new classifier..." << endl;
			#endif
			vector<string> filepaths;
			if(!fileExists(dataPath)){
				CV_Error( -1, "Could not find classifier data: " + dataPath);
			}
			CrawlFileTree(dataPath, filepaths);//TODO: Check if this dir exists and show a log message if not.
			
			/*
			#ifdef DEBUG_PROCESSOR
				cout << dataPath << endl;
				cout << acutal_classifier_size.width << endl;
				cout << filepaths.size() << endl;
				cout << classifiers.size() << endl;
			#endif
			*/

			const Json::Value advanced = classifier.get("advanced", Json::Value());
			bool success = classifiers[key]->train_PCA_classifier( filepaths,
				                                               acutal_classifier_size,
				                                               advanced.get("eigenvalues", 9).asInt(),
			                                                       advanced.get("flip_training_data", true).asBool());
			if( !success ) {
				//TODO: A better error message here when the training data isn't found would be a big help.
				LOGI("\n\nCould not train classifier.\n\n");
				return classifiers[key];
			}

			classifiers[key]->save(cachedDataPath);

			#ifdef DEBUG_PROCESSOR
				cout << "trained" << endl;
			#endif
		}
	}
	return classifiers[key];
}
Json::Value segmentFunction(Json::Value& segmentJsonOut, const Json::Value& extendedSegment) {
	Mat segmentImg;
	vector <Point> segBubbleLocs;
	vector <int> bubbleVals;
	Rect segmentRect( SCALEPARAM * Point(extendedSegment.get( "segment_x", INT_MIN ).asInt(),
	                                     extendedSegment.get( "segment_y", INT_MIN ).asInt()),
	                                     SCALEPARAM * Size(extendedSegment.get("segment_width", INT_MIN).asInt(),
	                                                       extendedSegment.get("segment_height", INT_MIN).asInt()));
	//Transfromation and offest are used to get absolute bubble locations.
	Mat transformation = (Mat_<double>(3,3) << 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0);
	Point offset = segmentRect.tl();

	//Get the cropped segment image:
	if(extendedSegment.get("align_segment", false).asBool()) {
		//Segment alignment is off by default because it seems to perform worse.
		Rect expandedRect = resizeRect(segmentRect, 1 + SEGMENT_BUFFER);
	
		//Reduce the segment buffer if it goes over the image edge.
		//TODO: see if you can do this with the rectangle intersection method.
		if(expandedRect.x < 0){
			expandedRect.x = 0;
		}
		if(expandedRect.y < 0){
			expandedRect.y = 0;
		}
		if(expandedRect.br().x > formImage.cols){
			expandedRect.width = formImage.cols - expandedRect.x;
		}
		if(expandedRect.br().y > formImage.rows){
			expandedRect.height = formImage.rows - expandedRect.y;
		}

		segmentImg = formImage(expandedRect);

		vector<Point2f> quad;
		findSegment(segmentImg, segmentRect - expandedRect.tl(), quad);

		if(testQuad(quad, segmentRect, .2)){
			#ifdef DEBUG_PROCESSOR
				//This makes a stream of dots so we can see how fast things are going.
				//we get a ! when things go wrong.
				cout << '.' << flush;
			#endif
			transformation = quadToTransformation(quad, segmentRect.size());
			offset = expandedRect.tl();
			Mat alignedSegment(0, 0, CV_8U);
			warpPerspective(segmentImg, alignedSegment, transformation, segmentRect.size());
			segmentImg = alignedSegment;
			segmentJsonOut["quad"] = quadToJsonArray( quad, offset );
		}
		else{
			#ifdef DEBUG_PROCESSOR
				//Bad quad alignment detected
				cout << "!" << flush;
			#endif
			//segmentJsonOut["quad"] = quadToJsonArray(quad, expandedRect.tl());
			segmentJsonOut["notFound"] = true;
			//If the quad is not found we don't do segment alignment and go with what's
			//in the template even though there's a good chance it's wrong.
			segmentImg = formImage(segmentRect);
			segmentJsonOut["quad"] = quadToJsonArray(rectToQuad( segmentRect ));
		}
	}
	else {
		segmentImg = formImage(segmentRect);
		segmentJsonOut["quad"] = quadToJsonArray(rectToQuad( segmentRect ));
	}

	//Do classification stuff:
	Json::Value items = extendedSegment["items"];
	if(!items.isNull()){
		Json::Value itemsJsonOut;
		Json::Value classifierJson = extendedSegment["classifier"];
		double alignment_radius = classifierJson.get("alignment_radius", 2.0).asDouble();
		Ptr<PCA_classifier> classifier = getClassifier(classifierJson);
		vector<Point> locations;
		vector<Point> deltas;
		//Align items:
		for (size_t i = 0; i < items.size(); i++) {
			Point initLocation = SCALEPARAM * Point(items[i]["item_x"].asDouble(), items[i]["item_y"].asDouble());
			Point itemLocation = initLocation;
			if(alignment_radius > .1){
				itemLocation = classifier->align_item(segmentImg, initLocation, alignment_radius);
			}
			locations.push_back(itemLocation);
			deltas.push_back(itemLocation - initLocation);

		}

		//Catch runaway items:
		Point avgDelta = Point(0,0);
		for (size_t i = 0; i < deltas.size(); i++) {
			avgDelta += deltas[i];
		}
		avgDelta *= 1.0 / items.size();

		for (size_t i = 0; i < locations.size(); i++) {
			if(norm(deltas[i] - avgDelta) > norm(deltas[i])){
				locations[i] = locations[i] - deltas[i] + avgDelta;
			}
		}

		//Classify items
		for (size_t i = 0; i < items.size(); i++) {
			Json::Value itemJsonOut = items[i];
			itemJsonOut["classification"] = classifier->classify_item(segmentImg, locations[i]);
			Mat absoluteLocation = transformation.inv() * Mat(Point3d( locations[i].x,
					locations[i].y, 1.0));
			itemJsonOut["absolute_location"] = pointToJson(
				Point( absoluteLocation.at<double>(0.0,0.0) / absoluteLocation.at<double>(2.0, 0.0),
				       absoluteLocation.at<double>(1.0,0.0) / absoluteLocation.at<double>(2.0, 0.0)) +
				offset);

			itemsJsonOut.append(itemJsonOut);
		}
		segmentJsonOut["items"] = itemsJsonOut;
	}  else if(extendedSegment.get("type", 0) == "qrcode"){
		LOGI("scanning qr code...");
		Mat tmp;
		//Blowing up the image makes decoding more accurate.
		//It seems like there should be a better way...
		resize(segmentImg, tmp, 4*segmentImg.size());
		//I modifieid this zxing class to take OpenCV mats
		Ref<LuminanceSource> source = ImageReaderSource::create(tmp);
		//LOGI("source created..");
		string result;
		try {
			Ref < Binarizer > binarizer;
			binarizer = new HybridBinarizer(source);
			DecodeHints hints(DecodeHints::DEFAULT_HINT);
			//Not sure if this makes any difference.
			hints.setTryHarder(true);
			Ref < BinaryBitmap > binary(new BinaryBitmap(binarizer));
			Ref<Reader> reader(new MultiFormatReader);
			vector<Ref<Result> > results(1, reader->decode(binary, hints));
			//LOGI("decoded!");
			result = results[0]->getText()->getText();
		} catch (const ReaderException& e) {
			//This happens when the qrcode is not recognized.
			result = string(e.what());
		} catch (const zxing::IllegalArgumentException& e) {
			result = "zxing::IllegalArgumentException: "
					+ string(e.what());
		} catch (const zxing::Exception& e) {
			result = "zxing::Exception: " + string(e.what());
		} catch (const std::exception& e) {
			result = "std::exception: " + string(e.what());
		}
		segmentJsonOut["value"] = result;
	}
	
	//Output the segment image:
	Mat segment_out, tmp;
	cvtColor(segmentImg, segment_out, CV_GRAY2RGB);
	resize(segment_out, tmp, 2*segment_out.size());
	segment_out = tmp;

	string segmentOutPath;
	string segmentName;
	try{
		segmentOutPath = extendedSegment.get("output_path", 0).asString() + "segments/";
		segmentName = extendedSegment["name"].asString() + "_image_" +
		              intToStr(extendedSegment.get("index", 0).asInt()) + ".jpg";

		/*
		rectangle(segment_out, expectedBubbleLocs[i] - .5 * classifier_size,
		          expectedBubbleLocs[i] + .5 * classifier_size,
		          colors[bubbleVals[i]]);

		circle(segment_out, segBubbleLocs[i], 1, Scalar(255, 2555, 255), -1);
		*/

		imwrite(segmentOutPath + segmentName, segment_out);
		segmentJsonOut["image_path"] = segmentOutPath + segmentName;
	}
	catch(...){
		LOGI(("Could not output segment to: " + segmentOutPath+segmentName).c_str());
	}
	return segmentJsonOut;
}
Json::Value fieldFunction(const Json::Value& field, const Json::Value& parentProperties){
	Json::Value extendedField = Json::Value(parentProperties);
	extend(extendedField, field);
	const Json::Value segments = extendedField["segments"];
	Json::Value outField = Json::Value(field);
	Json::Value outSegments;

	//This field is just used to block out features from some region.
	if(field.get("mask", false).asBool()) {
		cout << "mask" << endl;
		return Json::Value();
	}

	#ifdef OUTPUT_BUBBLE_IMAGES
		namer.setPrefix(field.get("classifier", Json::Value()).get("training_data_uri", "na").asString());
		//namer.setPrefix(field.get("label", "unlabeled").asString());
	#endif

	for ( size_t j = 0; j < segments.size(); j++ ) {
		const Json::Value segment = segments[j];
		Json::Value segmentJsonOut(segment);
		segmentJsonOut["index"] = (int)j;

		Json::Value extendedSegment = Json::Value(extendedField);
		extend(extendedSegment, segmentJsonOut);

		segmentJsonOut = segmentFunction(segmentJsonOut, extendedSegment);

		if(!segmentJsonOut.isNull()){
			outSegments.append(segmentJsonOut);
		}
	}
	outField["segments"] = outSegments;

	Json::Value value = computeFieldValue(outField);
	if(!value.isNull()){
		outField["value"] = value;
	}
	outField.removeMember("fields");
	outField.removeMember("items");
	outField.removeMember("classifier");
	return outField;
}
Json::Value formFunction(const Json::Value& templateRoot){
	const Json::Value fields = templateRoot["fields"];
	Json::Value outForm = Json::Value(templateRoot);
	Json::Value outFields;
	for ( size_t i = 0; i < fields.size(); i++ ) {
		const Json::Value field = fields[i];
		Json::Value outField = fieldFunction(field, templateRoot);
		if(!outField.isNull()){
			outFields.append(outField);
		}
	}
	outForm["fields"] = outFields;
	outForm["form_scale"] = SCALEPARAM;
	outForm["timestamp"] = currentDateTime();
	outForm.removeMember("items");
	outForm.removeMember("classifier");
	return outForm;
}

public:

ProcessorImpl()  {}

bool setTemplate(const char* templatePathArg) {
	#ifdef DEBUG_PROCESSOR
		cout << "setting template..." << endl;
	#endif
	//TODO: Remove templPath variable?
	templPath = addSlashIfNeeded(templatePathArg);
	bool success = parseJsonFromFile(templPath + "template.json", root);
	//root["template_path"] = templPath;
	return success;
}
bool loadFormImage(const char* imagePath, const char* calibrationFilePath) {
	#ifdef DEBUG_PROCESSOR
		cout << "loading form image..." << flush;
	#endif
	#ifdef TIME_IT	
		init = clock();
	#endif
	Mat temp;
	
	formImage = imread(imagePath, 0);
	if(formImage.empty()) return false;
	
	//Automatically rotate90 if the image is wider than it is tall
	//We want to keep the orientation consistent because:
	//1. It seems to make a slight difference in alignment. (SURF is not *completely* rotation invariant)
	//2. Undistortion
	//Maybe need to watch out incase a clock-wise rotation results in upsidown photos on some phones.
	if(formImage.cols > formImage.rows){
		transpose(formImage, temp);
		flip(temp,formImage, 1);
	}

	if(calibrationFilePath){
		Mat cameraMatrix, distCoeffs;
		Mat map1, map2;
		Size imageSize = formImage.size();
		
		//string calibPathString(calibrationFilePath);
		
		if( !fileExists(calibrationFilePath) ) return false;
		
		FileStorage fs(calibrationFilePath, FileStorage::READ);
		fs["camera_matrix"] >> cameraMatrix;
		fs["distortion_coefficients"] >> distCoeffs;
		
		initUndistortRectifyMap(cameraMatrix, distCoeffs, Mat(),
		                        getOptimalNewCameraMatrix(cameraMatrix,
		                                                  distCoeffs, imageSize, 0, imageSize, 0),
		                        imageSize, CV_16SC2, map1, map2);

		remap(formImage, temp, map1, map2, INTER_LINEAR);
		formImage = temp;
	}
/*
	string path = string(imagePath);
	imageDir = path.substr(0, path.find_last_of("/") + 1);
*/	
	#ifdef TIME_IT
		LOGI("LoadFormImage time: ");
		ostringstream ss;
		ss << (double)(clock()-init) / ((double)CLOCKS_PER_SEC);
		LOGI( ss.str().c_str() );
	#endif
	#ifdef DEBUG_PROCESSOR
		cout << "loaded" << endl;
	#endif
	return true;
}
bool alignForm(const char* alignedImageOutputPath, size_t formIdx) {
	#ifdef DEBUG_PROCESSOR
		cout << "aligning form..." << endl;
	#endif
	#ifdef TIME_IT	
		init = clock();
	#endif
	Mat straightenedImage;

	Size form_sz(root.get("width", 0).asInt(), root.get("height", 0).asInt());

	if( form_sz.width <= 0 || form_sz.height <= 0)
		CV_Error(CV_StsError, "Invalid form dimension in template.");

	//If the image was not set (because form detection didn't happen) set it.
	if( aligner.currentImg.empty() ) aligner.setImage(formImage);

	aligner.alignFormImage( straightenedImage, SCALEPARAM * form_sz, formIdx );

	
	if(straightenedImage.empty()) {
		cout << "does this ever happen?" << endl;
		return false;
	}

	formImage = straightenedImage;	
	//JsonOutput["aligned_image_path"] = alignedImageOutputPath;

	#ifdef TIME_IT
		LOGI("alignForm time: ");
		ostringstream ss;
		ss << (double)(clock()-init) / ((double)CLOCKS_PER_SEC);
		LOGI( ss.str().c_str() );
	#endif
	#ifdef DEBUG_PROCESSOR
		cout << "aligned" << endl;
	#endif
	return writeFormImage(alignedImageOutputPath);
}
bool processForm(const string& outputPath, const string& jsonOutputPath, const string& markedupImagePath, bool minifyJson) {
	#ifdef  DEBUG_PROCESSOR
		cout << "Processing form" << endl;
	#endif
	#ifdef TIME_IT	
		init = clock();
	#endif
	
	if( !root || formImage.empty() ){
		cout << "Unable to process form. Error code: " <<
		        (int)!root << (int)formImage.empty() << endl;
		return false;
	}
	root["output_path"] = outputPath;
	const Json::Value JsonOutput = formFunction(root);

	#ifdef  DEBUG_PROCESSOR
		cout << "done" << endl;
	#endif
	
	#ifdef  DEBUG_PROCESSOR
		cout << "outputting bubble vals..." << endl;
	#endif
	
	//Create the marked up image:
	imwrite(markedupImagePath, markupForm(JsonOutput, formImage, true));
	
	//Create the json output file
	ofstream outfile(jsonOutputPath.c_str(), ios::out | ios::binary);
	if(minifyJson){
		Json::FastWriter writer;
		outfile << writer.write( minifyJsonOutput(JsonOutput) );
	}
	else{
		outfile << JsonOutput;
	}
	outfile.close();

	#ifdef TIME_IT
		LOGI("Process time: ");
		ostringstream ss;
		ss << (double)(clock()-init) / ((double)CLOCKS_PER_SEC);
		LOGI( ss.str().c_str() );
	#endif
	return true;
}
//Writes the form image to a file.
bool writeFormImage(const char* outputPath) {
	return imwrite(outputPath, formImage);
}
bool loadFeatureData(const char* templatePathArg) {
	string templatePath = addSlashIfNeeded(templatePathArg);
	aligner.loadFeatureData(templatePath + "form.jpg",
							templatePath + "template.json",
							templatePath + "cached_features.yml");

	return true;
}
int detectForm(){
	int formIdx;
	try{
		LOGI("Detecting form...");
		aligner.setImage(formImage);
		formIdx = (int)aligner.detectForm();
	}
	catch(cv::Exception& e){
		LOGI(e.what());
		return -1;
	}
	return formIdx;
}
};

/* This stuff hooks the Processor class up to the implementation class: */
Processor::Processor() : processorImpl(new ProcessorImpl()){
	processorImpl->trainingDataPath = "training_examples/";
	LOGI("Processor successfully constructed.");
}
Processor::Processor(const char* appRootDir) : processorImpl(new ProcessorImpl()){
	processorImpl->trainingDataPath = addSlashIfNeeded(appRootDir) + "training_examples/";
	LOGI("Processor successfully constructed.");
}
bool Processor::loadFormImage(const char* imagePath, const char* calibrationFilePath){
	return processorImpl->loadFormImage(imagePath, calibrationFilePath);
}
bool Processor::loadFeatureData(const char* templatePath){
	try{
		return processorImpl->loadFeatureData(templatePath);
	}
	catch(cv::Exception& e){
		LOGI(e.what());
		return false;
	}
}
int Processor::detectForm(){
	return processorImpl->detectForm();
}
bool Processor::setTemplate(const char* templatePath){
	return processorImpl->setTemplate(templatePath);
}
bool Processor::alignForm(const char* alignedImageOutputPath, int formIdx){
	if(formIdx < 0) return false;
	try{
		return processorImpl->alignForm(alignedImageOutputPath, (size_t)formIdx);
	}
	catch(cv::Exception& e){
		LOGI(e.what());
		return false;
	}
}
bool Processor::processForm(const char* outputPath, bool minifyJson) {
	try{
		string normalizedOutDir = addSlashIfNeeded(outputPath);
		return processorImpl->processForm(normalizedOutDir, normalizedOutDir + "output.json",
				normalizedOutDir + "markedup.jpg", minifyJson);
	}
	catch(cv::Exception& e){
		LOGI(e.what());
		return false;
	}
}
const char* Processor::scanAndMarkup(const char* outputPath) {
	try{
		string normalizedOutDir = addSlashIfNeeded(outputPath);
		if(processorImpl->processForm(normalizedOutDir, normalizedOutDir + "output.json",
				normalizedOutDir + "markedup.jpg", false)) {
			return "";//success
		} else {
			return "Could not process form.";
		}
	}
	catch(cv::Exception& e){
		return e.what();
	}
}
/**
processViaJSON expects a JSON string like this:
{
	"inputImage" : "",
	"outputDirectory" : "",
	"alignedFormOutputPath" : outputDirectory + "aligned.jpg",
	"markedupFormOutputPath" : outputDirectory + "markedup.jpg",
	"jsonOutputPath" : outputDirectory + "output.json",
	"alignForm" : true,
	"processForm" : true,
	"templatePath" : "",
	"templatePaths" : [],
	"calibrationFilePath" : "",
	"trainingDataDirectory" : "training_examples/"
}
It will return a error string if there is an error, or an empty string if everything is OK.
You only need to specify the inputImage, outputDirectory and templatePath(s).
The JSON above contains all the default values.

The benefits are as follows:
- Only a single call is required to use the whole processing pipeline.
- Keyword arguments make it clearer what the args do,
  and allow for more flexible default behavior when they are not specified.
- Extra data can be passed in by the caller that is not yet supported here in the core without needing by modify the interface.
*/
const char* Processor::processViaJSON(const char* jsonString) {
	try {
		Json::Value config;// will contain the root value after parsing.
		Json::Reader reader;
		bool parsingSuccessful = reader.parse( jsonString, config );
		if(!parsingSuccessful){
			return "Could not parse JSON configuration string.";
		}
		if(!config.isMember("inputImage")){
			return "Missing input image path.";
		}
		if(!config.isMember("outputDirectory")){
			return "Missing output image path.";
		}
		string outputDirectory = config["outputDirectory"].asString();
		processorImpl->loadFormImage(config["inputImage"].asString().c_str(), config.get("calibrationFilePath", "").asString().c_str());

		int formIdx = 0;

		if(config.isMember("templatePath")){
			if(!processorImpl->loadFeatureData(config["templatePath"].asString().c_str())) {
				return "Could not load feature data.";
			}
			if(!processorImpl->setTemplate(config["templatePath"].asString().c_str())) {
				return "Could not set template.";
			}
		} else if(config.isMember("templatePaths")) {
			Json::Value templatePaths = config["templatePaths"];
			for ( size_t j = 0; j < templatePaths.size(); j++ ) {
				const Json::Value templatePath = templatePaths[j];
				if(!processorImpl->loadFeatureData(templatePath.asString().c_str())) {
					return "Could not load feature data.";
				}
			}
			formIdx = processorImpl->detectForm();
			if(formIdx < 0) {
				return "Could not detect form.";
			}
			if(!processorImpl->setTemplate(templatePaths[formIdx].asString().c_str())) {
				return "Could not set template.";
			}
		} else {
			return "One or more template paths are required.";
		}

		if(config.get("alignForm", true).asBool()){
			string alignedFormOutputPath = config.get("alignedFormOutputPath",
					addSlashIfNeeded(outputDirectory) + "aligned.jpg").asString();
			if(!processorImpl->alignForm(alignedFormOutputPath.c_str(), (size_t)formIdx)){
				return "Could not align form.";
			}
		}
		if(config.get("processForm", true).asBool()){
			processorImpl->trainingDataPath = config.get("trainingDataDirectory", "training_examples/").asString();
			string normalizedOutDir = addSlashIfNeeded(outputDirectory);
			string jsonOutputPath = config.get("jsonOutputPath",
					normalizedOutDir + "output.json").asString();
			string markedupFormOutputPath = config.get("markedupFormOutputPath",
					normalizedOutDir + "markedup.jpg").asString();
			if(!processorImpl->processForm(normalizedOutDir, jsonOutputPath, markedupFormOutputPath, false)){
				return "Could not process form.";
			}
		}
		return "";//Success
	}
	catch(cv::Exception& e){
		return e.what();
	}
}
bool Processor::writeFormImage(const char* outputPath) const{
	return processorImpl->writeFormImage(outputPath);
}
