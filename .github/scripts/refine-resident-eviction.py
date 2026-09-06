from pathlib import Path


def replace_once(path, old, new):
    p = Path(path)
    text = p.read_text()
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{path}: expected one match, found {count}")
    p.write_text(text.replace(old, new, 1))


replace_once(
    'sources/backend/PostRepositoryResidency.cpp',
    '''    --(*it);
    if (*it <= 0) {
        residentLeaseCounts.erase(it);
    }
    if (residentAccountedBytes > configuredHardBytes()) {
''',
    '''    --(*it);
    if (*it <= 0) {
        residentLeaseCounts.erase(it);
        auto state = residentBodies.find(key);
        if (state != residentBodies.end()) {
            // Idle TTL starts when the last active consumer lets go, not when a
            // long-lived widget/edit session first acquired its lease. Pressure
            // eviction remains independent of TTL.
            state->touchedAt = QDateTime::currentMSecsSinceEpoch();
        }
    }
    if (residentAccountedBytes > configuredHardBytes()) {
''')

replace_once(
    'tests/LongListWidgetTest.cpp',
    '''    void viewportLockScalesRelativeYAcrossResize()
    {
''',
    r'''    void bodyAvailabilityDropRerequestsSameLogicalBlock()
    {
        TestLongListWidget list;
        list.resize(480, 320);
        list.setDefaultItemHeight(60);
        list.setRequestBlockSize(10);
        list.setItemCount(100);
        list.setRangeAvailable(0, 99);
        list.show();
        settleEvents();

        list.scrollToIndex(55, Mattermost::LongListWidget::Alignment::Center);
        settleEvents(12);
        QVERIFY2(list.itemWidget(55) != nullptr,
                 "The target must be materialized before simulating body eviction");

        QSignalSpy requests(&list, &Mattermost::LongListWidget::rangeRequested);
        list.setRangeAvailable(55, 55, false);
        settleEvents(12);

        QVERIFY(!list.isItemAvailable(55));
        QCOMPARE(list.itemWidget(55), nullptr);

        bool requestedIdentityBlock = false;
        for (int i = 0; i < requests.count(); ++i) {
            const QList<QVariant> request = requests.at(i);
            if (request.at(0).toInt() == 50 && request.at(1).toInt() == 59) {
                requestedIdentityBlock = true;
                break;
            }
        }
        QVERIFY2(requestedIdentityBlock,
                 "Dropping only a resident body must re-request its existing 10-item logical block");

        // Rematerialization restores body availability only. No item-count or
        // structural mutation is needed for the same semantic source identity.
        list.setRangeAvailable(55, 55, true);
        settleEvents(12);
        QVERIFY(list.isItemAvailable(55));
        QVERIFY2(list.itemWidget(55) != nullptr,
                 "Restoring the body must materialize the same logical item again");
    }

    void viewportLockScalesRelativeYAcrossResize()
    {
''')

print('resident eviction refinements applied')
