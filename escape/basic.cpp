std::mutex mtx;
std::condition_variable cv;
std::queue<AVFrame*> frameQueue;
bool finished = false;

while (!finished) {
    char buf[65536];
    int ret = receiver.receive_and_assemble(buf, sizeof(buf), &ptime);
    if (ret > 0 && decoder.decode(buf, ret)) {
        AVFrame* frame = decoder.getFrame();
        if (frame) {
            std::unique_lock<std::mutex> lock(mtx);
            frameQueue.push(av_frame_clone(frame)); // push clone
            cv.notify_one(); // wake up renderer
        }
    }
}

while (!finished) {
    std::unique_lock<std::mutex> lock(mtx);
    cv.wait(lock, [&]() { return !frameQueue.empty() || finished; });

    if (!frameQueue.empty()) {
        AVFrame* frame = frameQueue.front();
        frameQueue.pop();
        lock.unlock();

        renderer.render(frame);
        av_frame_free(&frame);
    }
}
