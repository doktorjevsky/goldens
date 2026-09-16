# OCR model notices

The model weights in this directory originate from PaddleOCR. Copyright in
the upstream weights is held by Baidu and/or the applicable PaddleOCR rights
holders. RapidOCR converted the models to ONNX. Both projects distribute these
works under the Apache License, Version 2.0, whose complete text is included
in `LICENSE.txt`.

Bundled artifacts:

- `en_PP-OCRv3_det_mobile.onnx`
  - Upstream model: `en_PP-OCRv3_det_mobile`
  - SHA-256: `ea07c15d38ac40cd69da3c493444ec75b44ff23840553ff8ba102c1219ed39c2`
  - Source: <https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.9.2/onnx/PP-OCRv4/det/en_PP-OCRv3_det_mobile.onnx>
- `latin_PP-OCRv3_rec_mobile.onnx`
  - Upstream model: `latin_PP-OCRv3_rec_mobile`
  - SHA-256: `e9d7a33667e8aaa702862975186adf2012e3f390cc0f9422865957125f8071cf`
  - Source: <https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.9.2/onnx/PP-OCRv4/rec/latin_PP-OCRv3_rec_mobile.onnx>
- `latin_dict.txt`
  - Upstream dictionary: `latin_dict.txt`
  - SHA-256: `8e6d4e3629788c35c31f7e530287d6147b549bb7a265bd6708bb281134429e2c`
  - Source: <https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.9.2/paddle/PP-OCRv4/rec/latin_PP-OCRv3_rec_mobile/latin_dict.txt>

Project and licensing references:

- PaddleOCR: <https://github.com/PaddlePaddle/PaddleOCR>
- RapidOCR: <https://github.com/RapidAI/RapidOCR>
- RapidOCR model registry: <https://github.com/RapidAI/RapidOCR/blob/main/python/rapidocr/default_models.yaml>

The ONNX files are converted representations of the PaddleOCR models. The
conversion and packaging do not transfer ownership of the upstream weights or
remove their attribution.
