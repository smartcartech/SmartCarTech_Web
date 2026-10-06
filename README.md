# Website SmartCarTech

Website được xuất bản bằng GitHub Pages với Jekyll. Không cần chạy `build.py`.

- Sửa nội dung riêng của trang ngay trong `index.html`, `product-landing.html` hoặc file hướng dẫn tương ứng ở thư mục gốc.
- Sửa header, footer và bảng so sánh lần lượt trong `_includes/header.html`, `_includes/footer.html` và `_includes/comparison.html`. Các trang dùng `{% include ... %}` để GitHub Pages tự ghép nội dung khi xuất bản.
- Sửa CSS trong `css/base-utilities.css` hoặc `css/site.css`. Các trang tải trực tiếp hai file này; không còn bundle cần tạo lại. Khi đổi CSS, cập nhật phiên bản `?v=` trong các trang để làm mới cache trình duyệt.

Các file HTML nguồn cần có phần front matter `---` ở đầu để Jekyll xử lý include. Mở trực tiếp file HTML bằng trình duyệt hoặc phục vụ bằng máy chủ tĩnh đơn giản sẽ không ghép include; cần xem bản xuất bản trên GitHub Pages hoặc chạy Jekyll khi muốn xem trước tại máy.
